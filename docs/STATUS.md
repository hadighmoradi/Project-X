<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Project status

Project X is an experimental compiled-language project. The current repository contains a C++17 frontend and a custom x64 native-code backend. The language, runtime, and development tools are still evolving.

## Implemented in the current compiler

- Source text can be parsed into a `CompilationUnit`.
- The frontend performs semantic and control-flow analysis for the implemented language constructs.
- The control-flow model represents statement branches and expression-level paths for short-circuit logical operators, including `&&` and `||`.
- The Windows x64 code-generation path emits AMD64 COFF object modules and a deterministic link plan. The platform linker produces the final PE32+ executable.
- The repository includes regression suites for the frontend, control-flow analysis, native code generation, and object emission.

## Recent development

Recent N114 work makes short-circuit expression paths explicit in the control-flow model. It preserves expression-edge identity, distinguishes analysis-feasible paths from paths needed by machine-code generation, and tracks native branch locations through code layout. The aim is to make control-flow and branch-target handling easier to validate and maintain.

See the [N114 expression control-flow milestone](milestones/N114_EXPRESSION_CONTROL_FLOW_2026-11-27.md) for the technical overview.

## Still in development

- A complete, stable language specification and the full set of language features.
- A modular runtime and standard library.
- Integrated IDE support and the visual Windows form designer.
- A complete source-to-native-ELF compiler path for Linux x64.
- x86-32 code generation and macOS native output.

## Platform direction

| Target | Current state |
|---|---|
| Windows x64 | The integrated compiler path emits AMD64 COFF objects and a link plan for PE32+ linking. |
| Linux x64 | ELF64 object/image components exist; a complete source-to-ELF compiler path is still in development. |
| X32 / x86-32 | Planned; not implemented in the current backend. |
| macOS | Future target; Mach-O output is not implemented. |

The project is not presented as a finished or production-certified language. Features described as goals or work in progress should not be treated as available compiler features.
