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

## About this repository

This repository contains the compiler and its development test targets. It is an experimental compiler implementation, not yet a complete packaged language distribution with a finished runtime, standard library, IDE, or visual form designer.

For an overview of the language and implemented compiler path, see [language design](LANGUAGE_DESIGN.md), [architecture](ARCHITECTURE.md), and [current status](STATUS.md).
