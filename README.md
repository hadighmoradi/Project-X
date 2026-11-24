<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

<div align="center">

# Project X

[🌐 Visit the live project site](https://hadighmoradi.github.io/Project-X/)

### An experimental language and native toolchain

**C++17** · **CMake** · **x64 (x86-64)** · **Windows/Linux roadmap; macOS later**

<a href="README_FA.md">فارسی</a>

</div>

I started Project X to explore a compiled, native language and the tools around it. This repository snapshot contains **Native Frontend v1.48.0**. The work is still in progress; this is not a finished product.

## What exists in this snapshot

- A C++17 frontend and a currently active x64 (x86-64) native code-generation path.
- Parsing through `parse_source`, separate from `compile_x64_coff_program`, which accepts parsed `CompilationUnit` values.
- Windows x64 COFF object-module generation and a deterministic link plan; the platform linker creates the final PE executable.
- CMake-based build targets and a substantial regression-test suite.

## Target platform roadmap and current support

My plan is to support **x86-32 (X32) and x86-64 (X64)** on Windows and Linux, with macOS later. This is a roadmap, not a claim that every target works today.

| Target | Evidence in the current workspace |
|---|---|
| Windows x64 (x86-64) | Implemented compiler path: `compile_x64_coff_program` emits AMD64 COFF objects and a link plan; a platform linker produces the PE32+ executable. |
| Linux x64 (x86-64) | ELF64 object/image components and scoped Linux-host LLD evidence exist. A complete source-to-native-ELF compiler path is not established by the current compile API. |
| X32 (x86-32) | Roadmap target; the current target model/backend does not expose an x86-32 code-generation path. Here X32 means 32-bit x86, not Linux's x32 ABI. |
| macOS | Future target. Some target metadata is modeled, but the current workspace does not serialize Mach-O or establish macOS validation. |

“AMD64” is a conventional technical name for the x86-64 architecture and the COFF machine type used by the current Windows path; it does not mean the project is restricted to AMD processors. Build-host portability and generated-target support are separate questions.

## Project vision and name

I started this project because I want fast programs and small native executables without making code hard to write. The compiler is still experimental, so these are goals to work toward, not results I can claim today.

I call the project **X** for now; its final name is still open. The repository is public and GitHub Discussions are open. You can suggest a name in [Ideas](https://github.com/hadighmoradi/Project-X/discussions/categories/ideas), one idea per discussion, and briefly explain why it fits. After the community has discussed the suggestions, I will shortlist candidates and open a poll in [Polls](https://github.com/hadighmoradi/Project-X/discussions/categories/polls). The name with the most votes when the poll closes will replace X. See [Project vision and naming](docs/PROJECT_VISION_AND_NAMING.md).

## Current checkpoint

**N113-G62R16 · 23 November 2026**

| Area | Status |
|---|---|
| Source integrity | All 14 files in the G62R16 source snapshot match the live workspace. |
| Bounded probe | G62R13 passed an isolated MSVC G2-on workload: 56 actions, 66 machine moves, 474 transfer bytes. |
| General edge insertion | Not ready; G11 is 0/7 and insertion was not performed. |
| Full regression | Not run for this checkpoint. |
| Next recorded step | G62R17: examine expression-temporary liveness and register ownership before designing opt-in routing. |

These results apply only to the checks named above. The CMake inventory count is not a count of tests run.

## Architecture

```text
X source
   │
   ▼
parse_source ──► CompilationUnit
                    │
                    ▼
       compile_x64_coff_program
                    │
                    ▼
        AMD64 COFF objects + link plan
                    │
                    ▼
             platform linker
```

The active backend is custom x64 (x86-64) code, not LLVM. The current Windows path uses AMD64 COFF objects; the platform toolchain links the final executable. See the platform matrix above for the distinction between current support and roadmap targets.

## Build and test

See [Getting started](docs/GETTING_STARTED.md) for the CMake commands and scope of the registered test. They are documented commands, not a claim that this snapshot was built during preparation.

## Development history

The public-facing milestone summary is in [Development history](docs/DEVELOPMENT_HISTORY.md). Detailed patch manifests and host-validation evidence remain in the working archive; they are intentionally not copied into this source snapshot.

## Project status and architecture

- [Current engineering status](docs/STATUS.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Licensing policy and release gaps](docs/LICENSE_POLICY.md)
- [Live project page](https://hadighmoradi.github.io/Project-X/)
- [Project vision and naming](docs/PROJECT_VISION_AND_NAMING.md)
- [Readable project guide](https://hadighmoradi.github.io/Project-X/project-guide.html)

## Licensing

- Compiler, backend, tests, development tools, build scripts, and project-owned source fixtures: **GPL-3.0-or-later**.
- Project documentation and website content: **CC BY-SA 4.0**.
- Standalone examples are GPL-3.0-or-later unless an individual file explicitly declares another license, such as MIT.
- The runtime is not included in this snapshot. Its planned LGPL-3.0-or-later policy and X Runtime Linking Exception are not final or in effect yet.

The compiler's GPL license does not automatically make every independently written program compiled with it GPL-licensed. See [the full licensing policy](docs/LICENSE_POLICY.md), [copyright scope](COPYRIGHT.md), and [contributor guide](CONTRIBUTING.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution requirements, file notices, third-party material, and the licenses that apply to code and documentation. When reporting engineering results, include the exact source revision, host toolchain, command, and test scope.


## Language design and community

Explore the [visual language design overview](language-design.html) for the ideas behind X, its intended syntax, the current compiler path, and the roadmap. The [development workflow](docs/DEVELOPMENT_WORKFLOW.md) records how completed and verified phases are documented and published.
