#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Hector Moradi
# SPDX-License-Identifier: GPL-3.0-or-later
"""Dependency-free structural verifier for the N30 PE32+ multi-import/unwind smoke image."""
from __future__ import annotations
import argparse
import struct
from pathlib import Path


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from('<H', data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from('<I', data, off)[0]


def u64(data: bytes, off: int) -> int:
    return struct.unpack_from('<Q', data, off)[0]


def cstring(data: bytes, off: int) -> str:
    end = data.find(b'\0', off)
    if end < 0:
        raise ValueError('unterminated PE string')
    return data[off:end].decode('ascii')


def verify(path: Path) -> list[str]:
    data = path.read_bytes()
    checks: list[str] = []
    if len(data) < 0x400 or data[:2] != b'MZ':
        raise ValueError('missing DOS MZ signature or truncated headers')
    pe = u32(data, 0x3C)
    if pe + 24 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('missing PE signature')
    coff = pe + 4
    if u16(data, coff) != 0x8664:
        raise ValueError('COFF Machine is not AMD64')
    section_count = u16(data, coff + 2)
    optional_size = u16(data, coff + 16)
    opt = coff + 20
    if u16(data, opt) != 0x20B or optional_size != 240:
        raise ValueError('Optional Header is not PE32+')
    entry_rva = u32(data, opt + 16)
    image_base = u64(data, opt + 24)
    section_alignment = u32(data, opt + 32)
    file_alignment = u32(data, opt + 36)
    size_image = u32(data, opt + 56)
    size_headers = u32(data, opt + 60)
    subsystem = u16(data, opt + 68)
    dll_chars = u16(data, opt + 70)
    count_dirs = u32(data, opt + 108)
    if section_alignment != 0x1000 or file_alignment != 0x200:
        raise ValueError('unexpected section/file alignment')
    if size_headers % file_alignment or size_image % section_alignment:
        raise ValueError('header/image size alignment mismatch')
    if subsystem != 3 or (dll_chars & 0x140) != 0x140:
        raise ValueError('expected Windows console subsystem, DYNAMIC_BASE and NX_COMPAT')
    if count_dirs != 16:
        raise ValueError('expected 16 PE data directories')
    checks.append('DOS MZ, PE signature, AMD64 COFF and PE32+ Optional Header: PASS')
    checks.append(f'ImageBase=0x{image_base:016x}; EntryRVA=0x{entry_rva:08x}; SizeOfImage=0x{size_image:08x}')

    sections: dict[str, dict[str, int]] = {}
    ordered_names: list[str] = []
    shoff = opt + optional_size
    for index in range(section_count):
        sh = shoff + index * 40
        name = data[sh:sh + 8].split(b'\0', 1)[0].decode('ascii')
        if name in sections:
            raise ValueError(f'duplicate PE section name: {name}')
        ordered_names.append(name)
        sections[name] = {
            'virtual_size': u32(data, sh + 8), 'virtual_address': u32(data, sh + 12),
            'raw_size': u32(data, sh + 16), 'raw_offset': u32(data, sh + 20),
            'characteristics': u32(data, sh + 36),
        }
        sec = sections[name]
        if sec['raw_size'] and (sec['raw_offset'] % file_alignment or sec['raw_size'] % file_alignment):
            raise ValueError(f'{name} raw alignment mismatch')
        if sec['raw_offset'] + sec['raw_size'] > len(data):
            raise ValueError(f'{name} raw bytes outside file')
    expected_order = ['.text', '.rdata', '.data', '.idata', '.reloc', '.pdata', '.xdata']
    if ordered_names != expected_order:
        raise ValueError(f'expected section order {expected_order}, got {ordered_names}')
    expected_chars = {
        '.text': 0x60000020, '.rdata': 0x40000040, '.data': 0xC0000040,
        '.idata': 0xC0000040, '.reloc': 0x42000040,
        '.pdata': 0x40000040, '.xdata': 0x40000040,
    }
    for name, value in expected_chars.items():
        if sections[name]['characteristics'] != value:
            raise ValueError(f'{name} section characteristics mismatch')
    expected_rvas = {'.text': 0x1000, '.rdata': 0x2000, '.data': 0x3000,
                     '.idata': 0x4000, '.reloc': 0x5000, '.pdata': 0x6000, '.xdata': 0x7000}
    for name, value in expected_rvas.items():
        if sections[name]['virtual_address'] != value:
            raise ValueError(f'{name} expected RVA 0x{value:x}')
    if sections['.pdata']['virtual_size'] != 12 or sections['.xdata']['virtual_size'] != 8:
        raise ValueError('unexpected .pdata/.xdata virtual size')
    checks.append('Seven sections: .text RX, .rdata R, .data/.idata RW, .reloc discardable, .pdata/.xdata R: PASS')

    def rva_offset(rva: int, width: int = 1) -> int:
        if rva < size_headers:
            off = rva
            if off + width <= len(data):
                return off
        for sec in sections.values():
            start = sec['virtual_address']
            delta = rva - start
            if 0 <= delta and delta + width <= sec['raw_size']:
                return sec['raw_offset'] + delta
        raise ValueError(f'RVA range 0x{rva:x}+0x{width:x} does not map to file data')

    dirs = opt + 112
    def directory(index: int) -> tuple[int, int]:
        return u32(data, dirs + index * 8), u32(data, dirs + index * 8 + 4)

    import_rva, import_size = directory(1)
    exception_rva, exception_size = directory(3)
    reloc_rva, reloc_size = directory(5)
    iat_rva, iat_size = directory(12)
    if (import_rva, import_size) != (0x4000, 60):
        raise ValueError('expected 2 Import Descriptors plus null terminator (60 bytes)')
    if (exception_rva, exception_size) != (0x6000, 12):
        raise ValueError('Exception Directory must identify one RUNTIME_FUNCTION in .pdata')
    if (reloc_rva, reloc_size) != (0x5000, 12):
        raise ValueError('Base Relocation Directory unexpected')
    if (iat_rva, iat_size) != (0x4070, 0x30):
        raise ValueError('IAT Data Directory must cover both DLL thunk arrays')
    idata_start = sections['.idata']['virtual_address']
    idata_end = idata_start + sections['.idata']['virtual_size']
    if not (idata_start <= import_rva and import_rva + import_size <= idata_end and
            idata_start <= iat_rva and iat_rva + iat_size <= idata_end):
        raise ValueError('Import Directory and IAT must lie inside writable .idata')
    checks.append('Import, Exception, Base Relocation and IAT Data Directories: PASS')

    imports: dict[str, dict[str, int]] = {}
    descriptor_off = rva_offset(import_rva, import_size)
    descriptor_count = 0
    while True:
        current = descriptor_off + descriptor_count * 20
        oft, timestamp, forwarder, dll_rva, ft = struct.unpack_from('<IIIII', data, current)
        if oft == timestamp == forwarder == dll_rva == ft == 0:
            break
        if not oft or not dll_rva or not ft:
            raise ValueError('Import Descriptor has a required zero field')
        dll = cstring(data, rva_offset(dll_rva))
        symbols: dict[str, int] = {}
        for index in range(4096):
            thunk_rva = oft + index * 8
            thunk = u64(data, rva_offset(thunk_rva, 8))
            iat_slot = ft + index * 8
            iat_value = u64(data, rva_offset(iat_slot, 8))
            if thunk == 0:
                if iat_value != 0:
                    raise ValueError(f'{dll} IAT terminator is not zero')
                break
            if thunk & 0x8000000000000000:
                key = f'ordinal:{thunk & 0xffff}'
            else:
                name_off = rva_offset(thunk, 3)
                name = cstring(data, name_off + 2)
                if iat_value != thunk:
                    raise ValueError(f'{dll}!{name} initial IAT value differs from ILT value')
                key = name
            if key in symbols:
                raise ValueError(f'duplicate import thunk {dll}!{key}')
            symbols[key] = iat_slot
        else:
            raise ValueError(f'{dll} thunk table lacks a null terminator')
        if dll.lower() in imports:
            raise ValueError(f'duplicate DLL descriptor for {dll}')
        imports[dll.lower()] = symbols
        descriptor_count += 1
        if descriptor_count > 16:
            raise ValueError('Import Directory lacks a bounded null descriptor')
    if descriptor_off + (descriptor_count + 1) * 20 > descriptor_off + import_size:
        raise ValueError('null Import Descriptor lies outside directory size')
    expected = {
        'ntdll.dll': {'NtYieldExecution'},
        'kernel32.dll': {'GetCurrentProcessId', 'RtlLookupFunctionEntry', 'ExitProcess'},
    }
    if imports != {k: imports[k] for k in expected} or set(imports) != set(expected):
        raise ValueError(f'imported DLL descriptors mismatch: {imports}')
    for dll, names in expected.items():
        if set(imports[dll]) != names:
            raise ValueError(f'{dll} imports mismatch: {sorted(imports[dll])}')
    checks.append('Two DLL descriptors (NTDLL/KERNEL32), four name imports, ILT/IAT terminators: PASS')

    text = sections['.text']
    text_off = rva_offset(text['virtual_address'], text['virtual_size'])
    code = data[text_off:text_off + text['virtual_size']]
    if entry_rva != text['virtual_address'] or code[:4] != bytes.fromhex('48 83 ec 28'):
        raise ValueError('entry point or Windows x64 stack/shadow-space prologue mismatch')
    sites = [
        (6, 0x15, imports['ntdll.dll']['NtYieldExecution'], 'CALL NTDLL!NtYieldExecution'),
        (12, 0x15, imports['kernel32.dll']['GetCurrentProcessId'], 'CALL KERNEL32!GetCurrentProcessId'),
        (33, 0x15, imports['kernel32.dll']['RtlLookupFunctionEntry'], 'CALL KERNEL32!RtlLookupFunctionEntry'),
        (53, 0x25, imports['kernel32.dll']['ExitProcess'], 'JMP KERNEL32!ExitProcess (success)'),
        (68, 0x25, imports['kernel32.dll']['ExitProcess'], 'JMP KERNEL32!ExitProcess (failure)'),
    ]
    for disp_off, modrm, target_iat, description in sites:
        if code[disp_off - 2:disp_off] != bytes((0xff, modrm)):
            raise ValueError(f'{description} opcode mismatch')
        displacement = struct.unpack_from('<i', code, disp_off)[0]
        instruction_end_rva = text['virtual_address'] + disp_off + 4
        if instruction_end_rva + displacement != target_iat:
            raise ValueError(f'{description} RIP-relative displacement targets wrong IAT slot')
    if code[40:42] != bytes.fromhex('74 0f') or code[42:47] != bytes.fromhex('b9 2a 00 00 00'):
        raise ValueError('unwind success test or success exit-code path is malformed')
    if code[47:51] != bytes.fromhex('48 83 c4 28') or code[62:66] != bytes.fromhex('48 83 c4 28'):
        raise ValueError('tail-jump paths must restore the entry stack before transferring control')
    if code[57:62] != bytes.fromhex('b9 2b 00 00 00') or code[72:74] != bytes.fromhex('0f 0b'):
        raise ValueError('unwind failure exit-code path or unreachable trap is malformed')
    checks.append('Three FF 15 CALLs and two FF 25 tail jumps target their correct IAT slots: PASS')
    checks.append('Startup reserves 0x28 bytes, probes KERNEL32!RtlLookupFunctionEntry, restores RSP before both tail jumps, exits 42/43: PASS')

    # AMD64 Exception Directory -> RUNTIME_FUNCTION -> UNWIND_INFO.
    pdata_off = rva_offset(exception_rva, exception_size)
    begin_rva, end_rva, unwind_rva = struct.unpack_from('<III', data, pdata_off)
    if (begin_rva, end_rva, unwind_rva) != (0x1000, 0x104a, 0x7000):
        raise ValueError(f'RUNTIME_FUNCTION tuple unexpected: {(begin_rva, end_rva, unwind_rva)!r}')
    if begin_rva >= end_rva or begin_rva < text['virtual_address'] or end_rva > text['virtual_address'] + text['virtual_size']:
        raise ValueError('RUNTIME_FUNCTION range is outside .text or empty')
    xdata_off = rva_offset(unwind_rva, 8)
    unwind = data[xdata_off:xdata_off + 8]
    if unwind != bytes.fromhex('01 04 01 00 04 42 00 00'):
        raise ValueError(f'UNWIND_INFO does not encode Version 1 / 4-byte prologue / UWOP_ALLOC_SMALL 0x28: {unwind.hex()}')
    version = unwind[0] & 0x7
    flags = unwind[0] >> 3
    prolog_size = unwind[1]
    count_codes = unwind[2]
    frame_reg = unwind[3] & 0x0f
    frame_offset = unwind[3] >> 4
    code_offset = unwind[4]
    operation = unwind[5] & 0x0f
    op_info = unwind[5] >> 4
    if (version, flags, prolog_size, count_codes, frame_reg, frame_offset,
        code_offset, operation, op_info) != (1, 0, 4, 1, 0, 0, 4, 2, 4):
        raise ValueError('UNWIND_INFO header or UWOP_ALLOC_SMALL encoding invalid')
    if code_offset > prolog_size or (count_codes & 1 and unwind[6:8] != b'\0\0'):
        raise ValueError('UNWIND_CODE ordering/alignment padding invalid')
    checks.append('Exception Directory: one sorted RUNTIME_FUNCTION for .text; .pdata record bounds: PASS')
    checks.append('UNWIND_INFO: Version 1, PrologSize 4, UWOP_ALLOC_SMALL, 0x28-byte stack allocation, aligned: PASS')

    # Base relocation: DIR64 for an absolute pointer in .data (followed by ABSOLUTE pad).
    reloc_off = rva_offset(reloc_rva, reloc_size)
    page, block_size = struct.unpack_from('<II', data, reloc_off)
    if page != sections['.data']['virtual_address'] or block_size != reloc_size or block_size != 12:
        raise ValueError('Base Relocation block page/size malformed')
    entries = [u16(data, reloc_off + off) for off in range(8, block_size, 2)]
    nonzero = [(entry >> 12, entry & 0x0fff) for entry in entries if entry >> 12]
    if nonzero != [(10, 0)] or entries[-1] != 0:
        raise ValueError(f'expected DIR64 at .data+0 and ABSOLUTE alignment pad, got {entries!r}')
    if u64(data, rva_offset(page, 8)) != image_base + sections['.text']['virtual_address']:
        raise ValueError('preferred-base absolute pointer stored in .data is incorrect')
    checks.append('Base Relocation Directory: IMAGE_REL_BASED_DIR64 plus ABSOLUTE padding: PASS')
    return checks


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    try:
        checks = verify(args.image)
    except Exception as exc:
        print(f'FAIL: {exc}')
        return 1
    print(f'PE32+ image: {args.image}')
    for check in checks:
        print(check)
    print('STRUCTURAL RESULT: PASSED')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
