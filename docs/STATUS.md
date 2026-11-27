<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Current engineering status

**Latest validation record:** N114 v10.63 · 27 November 2026  
**Result:** `PASS_FULL_REGRESSION` for the declared 175,976-test inventory  
**Evidence environment:** User-run Windows x64 · MSVC · NMake serial

## Full Regression result

| Measure | Result |
|---|---:|
| Declared inventory / passed | **175,976 / 175,976** |
| Valid batches | **26 / 26** |
| Failed tests | **0** |
| Globally unique test IDs | **175,976** |
| Internal / cross-batch duplicates | **0 / 0** |
| Windows build matrix | **9 / 9 targets, exit code 0** |
| Source hash guards | **7 / 7, zero failures** |

The result covers the full declared CMake inventory represented by the 26-batch regression plan. It does not claim coverage outside that inventory or certify every target, ABI/configuration, feature, or generated executable.

## Important source synchronization boundary

The regression was run by the user in a local Windows/MSVC workspace. No source Git commit SHA was captured by the runner. The seven guards establish the identities of seven named files only; they do not establish identity of the complete source tree.

The public `main` revision reviewed for this record was `a387301b5efd7251a9346f823e2396748557743a`. A read-only comparison found that this public tree and the local source archive/workspace candidate differ, including the absence of the N114 B13-B test file from that public tree and size differences in core source files. **This documentation update does not synchronize source code.** Do not treat this PASS as a claim that the current public `main` contains the exact source tested in Windows. Complete source-tree reconciliation and synchronization remain pending.

A dedicated source-sync branch has been reserved with a curated 194-file package. Read-only reconciliation found 179 header-only Git-blob matches, 14 substantive existing source changes, and one new N114 B13-B test. The bundle is not yet pushed, so the public repository is not yet synchronized. See the [source-tree audit](milestones/PROJECT_X_N114_SOURCE_TREE_RECONCILIATION_AUDIT_2026-11-27.md).

The detailed batch matrix, seven hashes, reconciliation rule, and limitations are recorded in the [N114 v10.63 Full Regression milestone](milestones/PROJECT_X_N114_V10_63_FULL_REGRESSION_WINDOWS_MSVC_2026-11-27.md). The prior [G62R18 validation record](G62R18_VALIDATION.md) remains historical and unchanged.

## N114 technical boundaries

- `G9=STATEMENT_EDGE_ONLY`.
- `G10=DO_NOT_REBUILD`.
- `G11=0/7`.
- `spill_call_crossing=true`.
- XMM non-empty remains `OPEN/BLOCKED_NOT_PROVEN_IMPOSSIBLE`.
- No Edge Bundle insertion, SpillSlot reservation, unwind/relocation changes, or language/type/cast/conversion changes are established by this regression record.
- End-to-end execution of a generated PE was not part of this validation.
- This PASS is not a release certification.

## Target platform roadmap

The owner-stated direction is x86-32 (X32) and x86-64 (X64) for Windows and Linux, with macOS planned later. Current code evidence supports a narrower statement:

| Target | Current status |
|---|---|
| Windows x64 | `compile_x64_coff_program` emits AMD64 COFF objects and a deterministic link plan; the platform linker produces PE32+. |
| Linux x64 | ELF64 components and scoped Linux-host LLD evidence exist; an end-to-end source-to-ELF compiler path is not established. |
| X32 / x86-32 | Not implemented in the current target model/backend. |
| macOS | Future; Mach-O serialization and macOS validation are not established. |

Here X32 means 32-bit x86, not Linux x32 ABI. AMD64 is the standard technical name for x86-64 in the current COFF path; it does not imply an AMD-only target. A C++ build working on a host is not evidence that this compiler can generate native output for that host's operating system.


## Community naming

X is the temporary working name; the final language name is open for community choice. The repository is public and Discussions are enabled. Post candidates in [Ideas](https://github.com/hadighmoradi/Project-X/discussions/categories/ideas); after discussion and shortlisting, I will open the poll in [Polls](https://github.com/hadighmoradi/Project-X/discussions/categories/polls). No final-name poll has been published yet. See [Project vision and naming](PROJECT_VISION_AND_NAMING.md).
