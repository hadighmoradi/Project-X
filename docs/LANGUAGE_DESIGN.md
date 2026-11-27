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

## Current compiler and next steps

The public compiler is an experimental C++17 frontend with a custom x64 backend. The integrated Windows x64 path parses source into a `CompilationUnit`, emits AMD64 COFF object modules, and provides a deterministic link plan for the platform linker.

Recent compiler work improves the representation of short-circuit logical expressions. The control-flow model now records the paths associated with `&&` and `||`, while keeping analysis reachability distinct from the paths required for native-code generation. The backend also tracks the native branch locations associated with those expression paths.

The larger language and toolchain are still being developed. The runtime, standard library, complete language specification, IDE, visual form designer, Linux source-to-ELF path, x86-32 backend, and macOS output are not complete.

See [current project status](STATUS.md), [architecture](ARCHITECTURE.md), and the [N114 expression control-flow milestone](milestones/N114_EXPRESSION_CONTROL_FLOW_2026-11-27.md).

## Help shape the language

What would you like to build with X? Which features matter most to you? What do you like or dislike about the languages you already use?

Join the [GitHub discussions](https://github.com/hadighmoradi/Project-X/discussions) and tell us what you think.

For the latest engineering notes, see [current status](STATUS.md), [architecture](ARCHITECTURE.md), and [development history](DEVELOPMENT_HISTORY.md).
