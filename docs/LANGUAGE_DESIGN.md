<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# The ideas behind Project X

I want X to feel comfortable to write, produce native programs, and stay useful for both everyday apps and lower-level work. The aim is simple: make programming easier without giving up control over the final program.

X is still being built. This page explains the direction and the ideas that have influenced the design. It does not mean every feature shown here is already available in the compiler.

## What matters most

**Easy to read.** Common code should be clear. The language should not make simple tasks feel complicated.

**Fast native programs.** Programs should compile to native output. We will compare performance with equivalent C and C++ programs instead of making claims without measurements.

**Small executables.** A small program should not need to carry a large runtime or libraries it never uses.

**Control when it is needed.** The language should support systems work, resource management, and Windows applications as the project grows.

## What we are learning from other languages

I have looked at languages and tools that do different things well. These are ideas to learn from, not languages that X is trying to copy.

| Language or tool | What I like about it | What I want to bring to X |
|---|---|---|
| **Visual Basic** | Friendly, easy-to-read syntax | Make everyday code feel simple and familiar |
| **C** | Direct access to the system and native performance | Keep low-level control available and use C as a performance reference |
| **Delphi / Lazarus** | A complete development environment and visual form designer | Make it easy to build Windows apps with an IDE and a visual designer |
| **Python** | Straightforward code and helpful high-level libraries | Make common tasks easy without requiring a Python-style interpreter |
| **Rust** | Careful handling of types, errors, and resource ownership | Make resource lifetime and errors clear and predictable |
| **Windows Batch / CMD** | Short commands for files, folders, and running programs | Give common system tasks simple names and easy-to-use APIs |
| **PureBasic** | A direct way to create GUI applications | Keep GUI code approachable |

C and C++ are also useful points of comparison for native performance and low-level control. The goal is not to imitate their syntax in every detail.

## A quick look at the code

Source files use the .src extension and UTF-8 text. Blocks use braces, statements end with a semicolon, and functions start with the function keyword.

    function Add(left, right) {
        return left + right;
    }

    function Welcome() {
        PrintLine "Welcome to X.";
    }

A condition uses the same block style:

    if (ready) {
        PrintLine "Ready";
    } else {
        PrintLine "Please wait";
    }

For Windows forms, the design uses two related files. The .frm file describes the form; the matching .src file contains the program code and event handlers.

    Form MainForm {
        Caption = "Customer List";
        ClientWidth = 700;
        ClientHeight = 450;

        Button SaveButton {
            Caption = "Save";
            OnClick = HandleSave;
        }
    }

This is the planned form format. The visual designer and its connection to the finished compiler are still being developed.

## Where speed and small output come from

We are not taking small file size from one particular language. It comes down to how the compiler and libraries are built.

The current design aims for native compilation, static linking by default, and a modular runtime. A program should include only the library parts it needs. Design files such as .frm should be handled during design or build time, not parsed by every finished program.

Static linking does not automatically make a program small. The compiler, linker, libraries, and actual program all affect the result. We will need repeatable tests to measure executable size, startup time, and execution speed.

## What the public compiler can do today

The current public code is an experimental C++17 frontend with a custom x64 backend. Its integrated Windows x64 path looks like this:

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
    AMD64 COFF objects + link plan
        |
        v
    Windows linker
        |
        v
    PE32+ executable

This is a real but limited part of the larger plan. It is not yet the complete X language, runtime, standard library, IDE, or form designer.

## Current status and next steps

The latest recorded validation is **G62R18 (24 November 2026)**, run against the local **N113-G62R16** workspace baseline.

- The N111, N112, and N113 focused MSVC suites passed **252/252 tests**: 23/23, 25/25, and 204/204, with zero failures and zero skipped cases.
- Build environment: Windows x64, MSVC 19.51.36248, CMake 4.4.4, NMake Makefiles, Debug with /Z7. Configure and build both exited 0.
- Sixteen named source/build/test files matched their expected SHA-256 values before and after the run.
- This closes the N113 operation-level spill/reload validation scope.
- General CFG edge insertion is still not ready: G11 is 0/7 and Edge Bundle insertion was not performed. Broader CFG Edge Bundle integration moves to N114.
- Full regression and end-to-end execution of a generated PE were not part of this validation.

The reported project inventory total (175,937) is not the number of tests run. G62R18 executed only the 252 cases in the three named suites. The result applies to the local workspace and its 16-file hash manifest; it is not a claim that the entire public repository at a particular Git commit was independently matched to that workspace.

See the [G62R18 validation record](G62R18_VALIDATION.md) for the test totals, exact environment, SHA-256 manifest, evidence-package digest, and limitations.

The platform plan is broader than today's implementation:

| Platform | Current state |
|---|---|
| Windows x64 | The integrated compiler path creates AMD64 COFF objects and a link plan; the Windows linker produces PE32+ |
| Linux x64 | Some ELF64 parts and limited LLD tests exist, but a complete source-to-ELF path is not established |
| x86-32 | Planned; not implemented in the current backend |
| macOS | A future goal; Mach-O output and macOS validation are not in place |

## Help shape the language

What would you like to build with X? Which features matter most to you? What do you like or dislike about the languages you already use?

Join the [GitHub discussions](https://github.com/hadighmoradi/Project-X/discussions) and tell us what you think.

For the latest engineering notes, see [current status](STATUS.md), [architecture](ARCHITECTURE.md), and [development history](DEVELOPMENT_HISTORY.md).
