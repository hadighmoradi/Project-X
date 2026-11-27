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

## Latest validated workspace regression

**N114 v10.63 · 27 November 2026 · `PASS_FULL_REGRESSION`**

| Measure | Result |
|---|---:|
| Declared inventory | **175,976 / 175,976 passed** |
| Valid batches | **26 / 26** |
| Failed tests / global duplicate IDs | **0 / 0** |
| Windows build matrix | **9 / 9 targets, exit code 0** |
| Seven source hash guards | **7 / 7, zero failures** |

The N114 v10.63 source snapshot is now integrated into public `main`. Source commit `6e802faf646a301f124026d5d0661796dd97080b` was merged through [PR #7](https://github.com/hadighmoradi/Project-X/pull/7); the source merge commit is `320c8d737aa2dc7dd9989026c6ffa3781248e77b`. The curated bundle contained 194 files: all 193 previously tracked source paths plus the new N114 B13-B test. The source PR changed 15 paths (14 existing files and one new file).

The Full Regression result remains evidence for the user's local Windows/MSVC workspace. The runner did not record a source commit SHA, so this historical PASS is **not** claimed as a run against source commit `6e802faf646a301f124026d5d0661796dd97080b` or the merge commit. Validation against the exact published source revision remains pending; seven named hash guards do not establish full-tree test provenance.

See the [source-tree reconciliation and publication audit](docs/milestones/PROJECT_X_N114_SOURCE_TREE_RECONCILIATION_AUDIT_2026-11-27.md), the [public main synchronization record](docs/milestones/PROJECT_X_N114_PUBLIC_MAIN_SYNC_2026-11-27.md), and the [Full Regression milestone](docs/milestones/PROJECT_X_N114_V10_63_FULL_REGRESSION_WINDOWS_MSVC_2026-11-27.md). Historical G62R18 evidence remains available and has not been overwritten.

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



## Explore the language

See the [language design page](language-design.html) for the ideas behind X, a few code examples, and the current state of the compiler. The [development process](docs/DEVELOPMENT_WORKFLOW.md) explains how completed work and test results are recorded.
