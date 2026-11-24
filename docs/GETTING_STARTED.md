<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Getting started

## Requirements

- CMake 3.16 or newer.
- A C++17 compiler supported by CMake, such as MSVC, GCC, or Clang.
- On Windows, use a Visual Studio installation with the C++ workload when building with MSVC.

No third-party package download is declared by the top-level CMake configuration.

## Configure and build the registered test target

From the repository root:

```sh
cmake -S . -B build
cmake --build build --config Release --target xfrontend_tests
```

Run the CTest entry:

```sh
ctest --test-dir build -C Release --output-on-failure
```

The top-level CMake file registers `xfrontend_tests` as its CTest test. The repository also defines focused suite executables and smoke emitters; a normal CTest invocation does not imply that every historical host-validation script or milestone workload ran.

To build all configured targets, omit the target name:

```sh
cmake --build build --config Release
```

## Scope of the recorded evidence

The latest recorded focused validation is G62R18 (24 November 2026), against the local N113-G62R16 workspace baseline. The N111, N112, and N113 CMake suite targets were built with MSVC 19.51.36248 and CMake 4.4.4; all 252 selected tests passed (23 + 25 + 204), with zero failures and zero skipped cases.

This is not a full regression run. The recorded project inventory total of 175,937 is inventory metadata, not the executed test count. G11 remains 0/7, Edge Bundle insertion was not performed, and broader CFG Edge Bundle integration is assigned to N114.

The evidence applies to the named local workspace and 16-file before/after SHA-256 check; it does not by itself assert that the entire public repository tree at a Git commit is byte-identical to that workspace. See [G62R18 validation record](G62R18_VALIDATION.md) and [Current status](STATUS.md).
