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

The current documented checkpoint is N113-G62R16. It records a bounded G62R13 MSVC probe, a 14-file source-hash match, and a full regression that was not run for that checkpoint. See [Current status](STATUS.md) before interpreting test results.
