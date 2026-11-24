<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Project X language design overview

> A readable language, compiled to native programs, with a strong focus on predictable behavior and compact output.

This is a public, beginner-friendly summary of the direction documented in the project's architecture and syntax notes. It is not a complete language specification, and it does not claim that every designed feature is already implemented.

## What are we trying to build?

The long-term goal brings together three priorities:

1. **Approachable code:** everyday programs should be easy to read and write.
2. **Native execution:** compiled programs should run directly on the target operating system, with performance that can be compared honestly against C/C++ on equivalent workloads.
3. **Compact output:** executables should not carry an interpreter or unused library code just to use a small feature.

These are engineering goals. Actual speed, executable size, startup time, compatibility, and reliability must be measured on named builds and workloads before results are claimed.

## Where the design ideas come from

The project's design notes compare several established languages and tools. We take ideas as references, not their complete syntax, implementation, code, or license.

| Language or tool | What we learn from it | How that idea should appear in X |
|---|---|---|
| **Visual Basic** | Friendly, approachable syntax | Keep common code readable and avoid ceremony where it adds no value |
| **Python** | Clear high-level APIs and concise everyday operations | Make common library tasks straightforward without requiring a dynamic interpreter |
| **C / C++** | Native systems access, explicit control, streams, and a strong performance reference | Preserve low-level capability and measure native performance against equivalent C/C++ programs |
| **Rust** | Attention to types, explicit errors, ownership, and resource lifetime | Make resource behavior predictable and APIs clear about ownership and failure |
| **Windows Batch / CMD** | Direct operations for files, folders, environment variables, arguments, and processes | Offer simple named operations while avoiding fragile CMD parsing, implicit shell execution, and untyped text conventions |
| **PureBasic** | A simple, direct GUI programming experience | Aim for approachable GUI APIs and a visual form-design workflow |
| **Delphi / Lazarus** | Integrated development tools and visual form designers | Use the IDE and designer workflow as a product-design reference |

The latest compact architecture brief identifies Visual Basic, C, and Delphi/Lazarus as its headline inspirations. Older syntax comparison notes add Python, Rust, Windows Batch/CMD, and PureBasic for specific API and GUI comparisons. These references may evolve as the design is reviewed.

## A simple example of the intended syntax

The compact syntax reference uses `.src` source files, UTF-8 text, braces for blocks, semicolons at the end of statements, and the `function` keyword.

```x
function Add(left, right) {
    return left + right;
}

function PrintWelcome() {
    PrintLine "Welcome to X.";
}
```

A conditional follows the same block style:

```x
if (ready) {
    PrintLine "Ready";
} else {
    PrintLine "Please wait";
}
```

These examples explain the intended source style; they do not claim that the public compiler snapshot accepts every construct in the full design notes.

## Why we expect small native executables

**Small output is not a feature borrowed from one particular language.** It is an implementation goal supported by several engineering choices:

- Native compilation rather than requiring an interpreter to be installed for every program.
- Static linking as the documented default.
- A modular runtime and libraries, so only required pieces are selected for a build.
- Avoiding a mandatory runtime parser for design-time files such as `.frm`.
- Measuring executable size, startup time, and runtime overhead on repeatable test programs.

Static linking by itself does not guarantee a small executable. The actual result depends on the compiler, linker, selected libraries, runtime support, optimization, and the program being built.

## Native architecture: design goal versus current code

The long-term language pipeline is intended to turn source and design-time metadata into native program output. The current public repository demonstrates a narrower compiler path:

```text
Current public compiler path

Source text
   |
   v
parse_source
   |
   v
CompilationUnit
   |
   v
compile_x64_coff_program
   |
   v
AMD64 COFF object modules + deterministic link plan
   |
   v
Platform linker
   |
   v
Windows x64 PE32+ executable
```

The active backend in the public repository is custom x64 code; it is not LLVM. The compiler snapshot is experimental and does not yet represent the complete language, runtime, standard library, IDE, or GUI product.

## GUI and visual form design

The language design describes two related files:

- `.src`: the program's source code and event handlers.
- `.frm`: a declarative description of a form and its visual components.

A `.frm` file is intended as design-time input, not a general-purpose script that must be interpreted by every finished executable. The visual designer, component lifecycle, runtime integration, and production parser remain work to complete and validate.

## What works today?

**Public checkpoint: N113-G62R16 · 23 November 2026**

- The 14-file G62R16 source snapshot matches the corresponding live workspace files.
- A bounded G62R13 MSVC G2-on probe passed on its controlled workload: 56 actions, 66 machine moves, and 474 transfer bytes.
- General edge insertion is not ready: G11 is 0/7, and insertion was not performed.
- Full regression was not run for this checkpoint.
- The next recorded compiler task is G62R17: analyze expression-temporary liveness and register ownership before designing opt-in routing. It has not yet been performed.

Platform evidence is also limited:

| Target | Publicly documented state |
|---|---|
| Windows x64 | Current integrated path emits AMD64 COFF modules and a link plan; the platform linker produces PE32+ |
| Linux x64 | ELF64 components and scoped Linux-host LLD evidence exist; an end-to-end source-to-ELF compiler path is not established |
| x86-32 | Roadmap target; not implemented in the current backend |
| macOS | Future target; Mach-O output and macOS validation are not established |

## How the design will be updated

When a design decision is approved, the relevant language and architecture documents will be updated. When an engineering phase is completed and verified, its exact revision, environment, commands, tests, results, limitations, and next step will be recorded in the repository. A design proposal, a prototype, and a feature proven in the compiler must always be labelled separately.

See the [development workflow](DEVELOPMENT_WORKFLOW.md), [current status](STATUS.md), [architecture](ARCHITECTURE.md), and [development history](DEVELOPMENT_HISTORY.md).

## Join the discussion

What would you expect from a language you might actually use? Tell us what you want to build, which features matter most, and what you would improve in existing languages.

[Join the GitHub Discussions](https://github.com/hadighmoradi/Project-X/discussions)

---
*Public summary based on the Project X architecture brief and compact syntax reference dated 14 November 2026, alongside the current public repository checkpoint. Detailed proposals remain subject to review and change.*