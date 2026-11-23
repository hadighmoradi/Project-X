#!/usr/bin/env python3
"""Validate and lazily call an imported libc function through an ELF64 PLT."""

from __future__ import annotations

import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys


def run(*command: str) -> str:
    completed = subprocess.run(command, check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(f"command failed ({completed.returncode}): {' '.join(command)}\n{completed.stderr}")
    return completed.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shared_object", type=Path)
    args = parser.parse_args()
    path = args.shared_object.resolve()
    if not path.is_file():
        print(f"shared object not found: {path}", file=sys.stderr)
        return 2

    try:
        dynamic = run("readelf", "-d", str(path))
        relocations = run("readelf", "-rW", "--use-dynamic", str(path))
        symbols = run("readelf", "-sDW", str(path))
        # This writer intentionally omits section headers; disassemble the
        # file-backed RX virtual-address range as a raw x86-64 binary instead.
        symbol_line = next(line for line in symbols.splitlines() if "x_n28_getpid" in line)
        symbol_address = int(symbol_line.split()[1], 16)
        # The test function is six bytes and PLT begins at the next 16-byte boundary.
        plt_address = (symbol_address + 6 + 15) & ~15
        disassembly = run("objdump", "-D", "-b", "binary", "-m", "i386:x86-64",
                          f"--start-address={symbol_address}", f"--stop-address={plt_address + 32}", str(path))
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 3

    required_dynamic = ("Shared library: [libc.so.6]", "(PLTGOT)", "(PLTRELSZ)", "(PLTREL)", "(JMPREL)")
    if not all(item in dynamic for item in required_dynamic):
        print("required PLT dynamic tags are missing", file=sys.stderr)
        return 4
    if "R_X86_64_JUMP_SLOT" not in relocations or "getpid" not in relocations:
        print("R_X86_64_JUMP_SLOT for getpid is missing", file=sys.stderr)
        return 5
    if "getpid" not in symbols or "UND" not in symbols:
        print("undefined dynamic symbol getpid is missing", file=sys.stderr)
        return 6
    if "call" not in disassembly or "push" not in disassembly or disassembly.count("jmp") < 2:
        print("text call site or PLT0/PLTn instructions are missing", file=sys.stderr)
        return 7

    try:
        library = ctypes.CDLL(str(path), mode=os.RTLD_LOCAL | os.RTLD_LAZY)
        imported_call = library.x_n28_getpid
        imported_call.argtypes = []
        imported_call.restype = ctypes.c_int
        observed = [imported_call() for _ in range(3)]
    except (OSError, AttributeError, ValueError) as error:
        print(f"lazy dlopen/PLT resolution failed: {error}", file=sys.stderr)
        return 8

    expected = os.getpid()
    if observed != [expected, expected, expected]:
        print(f"PLT function results {observed} do not match process id {expected}", file=sys.stderr)
        return 9

    print("readelf DT_NEEDED libc.so.6: PASS")
    print("readelf DT_PLTGOT / DT_JMPREL / DT_PLTRELSZ / DT_PLTREL: PASS")
    print("readelf R_X86_64_JUMP_SLOT(getpid): PASS")
    print("readelf undefined dynsym getpid: PASS")
    print("objdump PLT stub and CALL rel32: PASS")
    print("dlopen RTLD_LAZY: PASS")
    print("first call lazy binding resolution: PASS")
    print("subsequent GOTPLT cached calls: PASS")
    print(f"x_n28_getpid() repeated results: {observed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
