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

The latest recorded Windows/MSVC regression for the local N114 v10.63 workspace passed all **175,976** tests in the declared inventory. All 26 batches and all nine build-matrix targets passed; no failed or duplicate test IDs were reported. See the [Full Regression milestone](milestones/PROJECT_X_N114_V10_63_FULL_REGRESSION_WINDOWS_MSVC_2026-11-27.md).

This is evidence from the user's local workspace, not proof that the public `main` tree at a specific commit is byte-identical to the tested source. The runner did not record a source commit SHA, and full public source synchronization is pending. This documentation does not claim that the currently published source has been rebuilt or re-tested by this record. G9 remains statement-edge-only, G10 remains do-not-rebuild, G11 remains 0/7, and generated-PE end-to-end execution is not established.
