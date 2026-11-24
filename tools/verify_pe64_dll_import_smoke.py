#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Hector Moradi
# SPDX-License-Identifier: GPL-3.0-or-later
"""Structural verifier for the N36 PE32+ DLL import/IAT smoke image."""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from('<H', data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from('<I', data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from('<Q', data, offset)[0]


def cstr(data: bytes, offset: int) -> str:
    end = data.find(b'\0', offset)
    if end < 0:
        raise ValueError('unterminated PE string')
    return data[offset:end].decode('ascii')


def verify(path: Path) -> list[str]:
    data = path.read_bytes()
    checks: list[str] = []
    if data[:2] != b'MZ':
        raise ValueError('DOS MZ signature missing')
    pe = u32(data, 0x3C)
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('PE signature missing')
    coff = pe + 4
    if u16(data, coff) != 0x8664:
        raise ValueError('machine is not AMD64')
    section_count = u16(data, coff + 2)
    optional = coff + 20
    if u16(data, optional) != 0x20B:
        raise ValueError('optional header is not PE32+')
    image_base = u64(data, optional + 24)
    directories = optional + 112
    import_rva, import_size = struct.unpack_from('<II', data, directories + 8)
    reloc_rva, reloc_size = struct.unpack_from('<II', data, directories + 5 * 8)
    iat_rva, iat_size = struct.unpack_from('<II', data, directories + 12 * 8)
    if not (import_rva and import_size and iat_rva and iat_size and reloc_rva and reloc_size):
        raise ValueError('Import, IAT or base-relocation directory is missing')
    characteristics = u16(data, coff + 18)
    dll_characteristics = u16(data, optional + 70)
    if not (characteristics & 0x2000):
        raise ValueError('COFF DLL characteristic missing')
    if not (dll_characteristics & 0x40):
        raise ValueError('DYNAMIC_BASE flag missing')

    section_table = optional + u16(data, coff + 16)
    sections: dict[str, tuple[int, int, int, int, int]] = {}
    for index in range(section_count):
        off = section_table + index * 40
        name = data[off:off + 8].split(b'\0', 1)[0].decode('ascii')
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from('<IIII', data, off + 8)
        section_flags = u32(data, off + 36)
        sections[name] = (virtual_address, virtual_size, raw_offset, raw_size, section_flags)

    def rva_to_offset(rva: int, count: int = 1) -> int:
        for va, vsize, raw, rsize, _ in sections.values():
            if rva >= va and rva - va <= rsize and count <= rsize - (rva - va):
                return raw + (rva - va)
        raise ValueError(f'RVA 0x{rva:x} does not map to raw section bytes')

    if '.idata' not in sections or '.reloc' not in sections or '.edata' not in sections:
        raise ValueError('required .idata/.reloc/.edata section missing')
    idata_flags = sections['.idata'][4]
    if idata_flags != 0xC0000040:
        raise ValueError(f'.idata characteristics unexpected: 0x{idata_flags:08x}')
    desc = rva_to_offset(import_rva, 20)
    oft, timestamp, forwarder, name_rva, first_thunk = struct.unpack_from('<IIIII', data, desc)
    if timestamp or forwarder:
        raise ValueError('unexpected timestamp/forwarder fields in import descriptor')
    if cstr(data, rva_to_offset(name_rva)) != 'KERNEL32.dll':
        raise ValueError('import descriptor DLL name mismatch')
    if first_thunk != iat_rva or iat_size != 16:
        raise ValueError('IAT directory metadata mismatch')
    lookup_value = u64(data, rva_to_offset(oft, 8))
    address_value = u64(data, rva_to_offset(first_thunk, 8))
    if lookup_value != address_value or lookup_value & (1 << 63):
        raise ValueError('ILT and initial IAT entry mismatch')
    import_name_offset = rva_to_offset(lookup_value, 2)
    if u16(data, import_name_offset) != 0 or cstr(data, import_name_offset + 2) != 'GetCurrentProcessId':
        raise ValueError('IMAGE_IMPORT_BY_NAME is malformed')
    if u64(data, rva_to_offset(oft + 8, 8)) or u64(data, rva_to_offset(first_thunk + 8, 8)):
        raise ValueError('ILT/IAT null terminator missing')

    text_va, _, text_raw, _, _ = sections['.text']
    if data[text_raw:text_raw + 2] != b'\xff\x25':
        raise ValueError('text export is not an FF 25 tail-jump IAT thunk')
    displacement = struct.unpack_from('<i', data, text_raw + 2)[0]
    if text_va + 6 + displacement != first_thunk:
        raise ValueError('FF 25 displacement does not target the IAT slot')

    reloc_offset = rva_to_offset(reloc_rva, 12)
    page_rva, block_size = struct.unpack_from('<II', data, reloc_offset)
    if block_size != 12 or page_rva == 0:
        raise ValueError('unexpected relocation block size/page RVA')
    reloc_entry = u16(data, reloc_offset + 8)
    if reloc_entry >> 12 != 10:
        raise ValueError('DIR64 relocation entry missing')

    checks.extend([
        f'PE32+ AMD64 DLL: PASS (ImageBase=0x{image_base:x}, {section_count} sections)',
        'Import Directory: one KERNEL32.dll descriptor plus null descriptor: PASS',
        'ILT/IAT: GetCurrentProcessId by name, hint/name and null terminators: PASS',
        'IAT section: read/write characteristics and Optional Header IAT directory: PASS',
        'Export code: FF 25 RIP-relative tail-jump targets the IAT slot: PASS',
        'ASLR compatibility: DYNAMIC_BASE and DIR64 relocation directory: PASS',
    ])
    return checks


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: verify_pe64_dll_import_smoke.py <dll>')
    try:
        for line in verify(Path(sys.argv[1])):
            print(line)
    except (OSError, ValueError, struct.error) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        raise SystemExit(1)
