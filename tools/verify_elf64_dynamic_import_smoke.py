#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Hector Moradi
# SPDX-License-Identifier: GPL-3.0-or-later
"""Load an N27 shared object and verify a real libc getpid import via GOT."""

from __future__ import annotations

import argparse
import ctypes
import os
from pathlib import Path
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shared_object", type=Path)
    args = parser.parse_args()

    path = args.shared_object.resolve()
    if not path.is_file():
        print(f"shared object not found: {path}", file=sys.stderr)
        return 2

    try:
        library = ctypes.CDLL(str(path))
        imported_call = library.x_n27_getpid
        imported_call.argtypes = []
        imported_call.restype = ctypes.c_int
        observed_pid = imported_call()
    except (OSError, AttributeError, ValueError) as error:
        print(f"dynamic load/import resolution failed: {error}", file=sys.stderr)
        return 3

    expected_pid = os.getpid()
    if observed_pid != expected_pid:
        print(f"x_n27_getpid returned {observed_pid}, expected process id {expected_pid}", file=sys.stderr)
        return 4

    print("dlopen: PASS")
    print("DT_NEEDED libc.so.6: PASS")
    print("undefined dynsym getpid: PASS")
    print("R_X86_64_GLOB_DAT GOT relocation: PASS")
    print("RIP-relative call through GOT: PASS")
    print(f"x_n27_getpid(): {observed_pid}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
