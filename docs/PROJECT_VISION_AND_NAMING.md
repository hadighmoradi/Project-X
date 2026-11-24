<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Project vision and name

I started Project X because I want a language that is easy to write, runs as a native program, and can produce small executables.

The goal is to bring together the friendly feel of a high-level language and the control of a native one. We want to make everyday code clear without taking away the tools needed for systems work.

These are the goals for the project. The compiler is still experimental, so speed, executable size, and ease of use must be tested as the language grows.

## Where the design ideas come from

The language design takes inspiration from several places:

- Visual Basic for readable, approachable syntax.
- C for native control and as a performance reference.
- Delphi and Lazarus for their IDEs and visual form designers.
- Python for clear, high-level ways to do common tasks.
- Rust for careful handling of types, errors, and resources.
- Windows Batch for simple file and system operations.
- PureBasic for an approachable way to build GUI programs.

We want to learn from what works well in each of them and make the result feel like one language.

See the [language design page](../language-design.html) for examples and the current compiler status.

## Help choose the name

X is only a working name. I would like the final name to come from the people who use and follow the project.

Have a name in mind? Start a discussion in [Ideas](https://github.com/hadighmoradi/Project-X/discussions/categories/ideas), one name per post, and tell us why you like it. We will talk through the suggestions and then put a shortlist in a poll under [Polls](https://github.com/hadighmoradi/Project-X/discussions/categories/polls).

The name with the most votes when the poll closes will become the new project name.

## Platforms

The long-term goal is to support x86-32 and x86-64 on Windows and Linux, with macOS considered for a later stage.

Today's public compiler is more limited:

| Platform | Current state |
|---|---|
| Windows x64 | The integrated path creates AMD64 COFF objects and a link plan; the Windows linker produces PE32+ |
| Linux x64 | ELF64 parts and limited LLD tests exist, but a complete source-to-ELF path is not established |
| x86-32 | Planned, but not implemented in the current backend |
| macOS | A future goal; Mach-O output and macOS validation are not ready |
