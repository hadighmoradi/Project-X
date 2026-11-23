#!/usr/bin/env python3
"""Structural verifier for the N37 PE32+ TLS Directory smoke image."""
from __future__ import annotations
import struct
import sys
from pathlib import Path


def u16(data: bytes, off: int) -> int: return struct.unpack_from('<H', data, off)[0]
def u32(data: bytes, off: int) -> int: return struct.unpack_from('<I', data, off)[0]
def u64(data: bytes, off: int) -> int: return struct.unpack_from('<Q', data, off)[0]


def verify(path: Path) -> list[str]:
    data = path.read_bytes()
    if data[:2] != b'MZ': raise ValueError('DOS signature missing')
    pe = u32(data, 0x3C)
    if data[pe:pe + 4] != b'PE\0\0': raise ValueError('PE signature missing')
    coff = pe + 4
    if u16(data, coff) != 0x8664: raise ValueError('machine is not AMD64')
    count = u16(data, coff + 2)
    optional = coff + 20
    if u16(data, optional) != 0x20B: raise ValueError('optional header is not PE32+')
    image_base = u64(data, optional + 24)
    entry_point_rva = u32(data, optional + 16)
    directories = optional + 112
    tls_rva, tls_size = struct.unpack_from('<II', data, directories + 9 * 8)
    import_rva, import_size = struct.unpack_from('<II', data, directories + 1 * 8)
    reloc_rva, reloc_size = struct.unpack_from('<II', data, directories + 5 * 8)
    iat_rva, iat_size = struct.unpack_from('<II', data, directories + 12 * 8)
    dll_chars = u16(data, optional + 70)
    if tls_rva == 0 or tls_size != 40: raise ValueError('TLS directory data-directory entry is missing')
    if not import_rva or import_size != 40 or not iat_rva or iat_size != 16:
        raise ValueError('N36 Import Directory/IAT metadata is missing or malformed')
    if not (dll_chars & 0x40) or not reloc_rva or not reloc_size: raise ValueError('ASLR/relocation metadata missing')
    section_table = optional + u16(data, coff + 16)
    sections: dict[str, tuple[int, int, int, int, int]] = {}
    for i in range(count):
        off = section_table + i * 40
        name = data[off:off + 8].split(b'\0', 1)[0].decode('ascii')
        vs, va, rs, ro = struct.unpack_from('<IIII', data, off + 8)
        flags = u32(data, off + 36)
        sections[name] = (va, vs, ro, rs, flags)

    def rva_off(rva: int, size: int = 1) -> int:
        for va, _vs, ro, rs, _flags in sections.values():
            if rva >= va and rva - va <= rs and size <= rs - (rva - va):
                return ro + (rva - va)
        raise ValueError(f'RVA 0x{rva:x} is not backed by section data')

    for required in ('.text', '.data', '.tls', '.edata', '.reloc'):
        if required not in sections: raise ValueError(f'{required} section missing')
    if sections['.tls'][4] != 0xC0000040: raise ValueError('.tls must be readable/writable initialized data')
    if sections['.data'][4] != 0xC0000040: raise ValueError('.data must be readable/writable')
    if entry_point_rva != sections['.text'][0] + 80:
        raise ValueError('DllMain AddressOfEntryPoint does not reference the expected .text stub')
    if data[rva_off(entry_point_rva, 6):rva_off(entry_point_rva, 6) + 6] != bytes((0xB8, 1, 0, 0, 0, 0xC3)):
        raise ValueError('DllMain stub must return TRUE')

    import_desc = rva_off(import_rva, 40)
    oft, timestamp, forwarder, dll_name_rva, first_thunk = struct.unpack_from('<IIIII', data, import_desc)
    if timestamp or forwarder or first_thunk != iat_rva:
        raise ValueError('KERNEL32 import descriptor fields are malformed')
    dll_name_off = rva_off(dll_name_rva)
    dll_name_end = data.find(b'\0', dll_name_off)
    if dll_name_end < 0 or data[dll_name_off:dll_name_end] != b'KERNEL32.dll':
        raise ValueError('TLS DLL did not preserve the KERNEL32.dll import descriptor')
    lookup_value = u64(data, rva_off(oft, 8))
    if lookup_value & (1 << 63): raise ValueError('expected named import, found ordinal thunk')
    ibn_off = rva_off(lookup_value, 2)
    if u16(data, ibn_off) != 0: raise ValueError('KERNEL32 import Hint mismatch')
    name_off = ibn_off + 2
    name_end = data.find(b'\0', name_off)
    if name_end < 0 or data[name_off:name_end] != b'GetCurrentProcessId':
        raise ValueError('KERNEL32 GetCurrentProcessId import name mismatch')
    if u64(data, rva_off(oft + 8, 8)) or u64(data, rva_off(first_thunk + 8, 8)):
        raise ValueError('Import Lookup Table / IAT terminator missing')

    directory = rva_off(tls_rva, 40)
    start, end, index_va, callbacks_va = struct.unpack_from('<QQQQ', data, directory)
    zero_fill, characteristics = struct.unpack_from('<II', data, directory + 32)
    tls_va, tls_vs, _tls_raw, _tls_rs, _tls_flags = sections['.tls']
    data_va, _data_vs, _data_raw, _data_rs, _data_flags = sections['.data']
    if start != image_base + tls_va or end != start + 4: raise ValueError('TLS raw-data VA range mismatch')
    if index_va != image_base + data_va + 4: raise ValueError('TLS AddressOfIndex mismatch')
    if zero_fill != 4 or characteristics != 0: raise ValueError('TLS zero-fill/characteristics mismatch')
    if data[rva_off(tls_va, 4):rva_off(tls_va, 4) + 4] != bytes((0x78, 0x56, 0x34, 0x12)):
        raise ValueError('TLS initial template content mismatch')
    if u32(data, rva_off(data_va + 4, 4)) != 0: raise ValueError('TLS index cell is not initially zero')
    if callbacks_va == 0: raise ValueError('TLS callback VA array missing')
    callbacks_rva = callbacks_va - image_base
    callback_va = u64(data, rva_off(callbacks_rva, 8))
    if callback_va != image_base + sections['.text'][0]: raise ValueError('TLS callback entry does not point to .text start')
    if u64(data, rva_off(callbacks_rva + 8, 8)) != 0: raise ValueError('TLS callback array lacks null terminator')

    reloc_targets: set[int] = set()
    cursor = rva_off(reloc_rva, reloc_size)
    end_off = cursor + reloc_size
    while cursor < end_off:
        if end_off - cursor < 8: raise ValueError('truncated relocation block')
        page, block_size = struct.unpack_from('<II', data, cursor)
        if block_size < 8 or block_size & 1 or block_size > end_off - cursor:
            raise ValueError('invalid relocation block size')
        for pos in range(cursor + 8, cursor + block_size, 2):
            entry = u16(data, pos)
            kind = entry >> 12
            if kind == 0: continue
            if kind != 10: raise ValueError(f'unexpected relocation type {kind}')
            reloc_targets.add(page + (entry & 0xFFF))
        cursor += block_size
    expected = {tls_rva, tls_rva + 8, tls_rva + 16, tls_rva + 24, callbacks_rva}
    if reloc_targets != expected: raise ValueError(f'TLS DIR64 targets mismatch: {sorted(reloc_targets)}')

    text_va, _text_vs, text_raw, _text_rs, _text_flags = sections['.text']
    marker_rva = data_va
    index_rva = data_va + 4
    callback_disp = struct.unpack_from('<i', data, text_raw + 3)[0]
    getter_disp = struct.unpack_from('<i', data, text_raw + 18)[0]
    setter_disp = struct.unpack_from('<i', data, text_raw + 52)[0]
    if text_va + 7 + callback_disp != marker_rva:
        raise ValueError('TLS callback RIP-relative marker counter displacement mismatch')
    if text_va + 22 + getter_disp != index_rva or text_va + 56 + setter_disp != index_rva:
        raise ValueError('TLS accessors do not reference the loader-owned TLS index cell')

    return [
        f'PE32+ AMD64 DLL: PASS ({count} sections, ImageBase=0x{image_base:x})',
        'TLS Data Directory: IMAGE_TLS_DIRECTORY64 (40 bytes): PASS',
        'TLS initialized template and zero-fill metadata: PASS',
        'Writable TLS index cell and callback VA array: PASS',
        'TLS callback target plus null terminator: PASS',
        'Optional DllMain entrypoint stub (returns TRUE): PASS',
        'N36 KERNEL32 import descriptor and IAT coexist with TLS: PASS',
        'TLS callback and accessors use the expected RIP-relative data/index cells: PASS',
        'ASLR: DYNAMIC_BASE and five TLS DIR64 relocations: PASS',
    ]


if __name__ == '__main__':
    if len(sys.argv) != 2: raise SystemExit('usage: verify_pe64_dll_tls_smoke.py <dll>')
    try:
        for item in verify(Path(sys.argv[1])): print(item)
    except (OSError, ValueError, struct.error) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        raise SystemExit(1)
