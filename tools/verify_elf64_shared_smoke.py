#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Hector Moradi
# SPDX-License-Identifier: GPL-3.0-or-later
"""Load an N26 smoke shared object and verify its exported RELATIVE pointer."""

from __future__ import annotations

import argparse
import ctypes
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
        answer = library.x_answer
        answer.argtypes = []
        answer.restype = ctypes.c_int
        value = answer()
        pointer = ctypes.c_void_p.in_dll(library, "x_answer_pointer").value
        function_address = ctypes.cast(answer, ctypes.c_void_p).value
    except (OSError, AttributeError, ValueError) as error:
        print(f"dynamic load or symbol lookup failed: {error}", file=sys.stderr)
        return 3

    if value != 42:
        print(f"x_answer returned {value}, expected 42", file=sys.stderr)
        return 4
    if pointer is None or pointer != function_address:
        print("R_X86_64_RELATIVE pointer does not equal the loaded function address", file=sys.stderr)
        return 5

    print("dlopen: PASS")
    print(f"x_answer(): {value}")
    print(f"relocated pointer: 0x{pointer:x}")
    print(f"function address:  0x{function_address:x}")
    print("R_X86_64_RELATIVE runtime check: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
