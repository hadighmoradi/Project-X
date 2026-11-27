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

## Source synchronization and validation provenance

The N114 v10.63 source snapshot is now present in public `main`. Source branch commit `6e802faf646a301f124026d5d0661796dd97080b` was integrated by [PR #7](https://github.com/hadighmoradi/Project-X/pull/7), producing merge commit `320c8d737aa2dc7dd9989026c6ffa3781248e77b`. The documentation checkpoint was integrated by [PR #6](https://github.com/hadighmoradi/Project-X/pull/6).

The curated source candidate comprised 194 manifest entries: 193 existing tracked source paths and one new N114 B13-B test. The source PR changed exactly 15 paths (14 modified and one added). The source-bundle SHA-256, ZIP structure, all 194 file SHA-256 values, all 194 staged Git blob IDs, and the exact 15-path staged delta passed pre-commit guards. The source branch read-back matched the local commit SHA before PR merge.

**The historical Full Regression result is not yet attributable to the published source commit.** The run came from the user's local Windows/MSVC workspace, and the runner did not record a source commit SHA. Seven file hash guards do not establish complete source-tree identity. Rebuild and rerun the required Windows/MSVC validation against exact source commit `6e802faf646a301f124026d5d0661796dd97080b` before claiming that the 175,976-test PASS applies to this published revision.

The GitHub status query found no commit status checks and no associated workflow runs for the PR heads. No CI pass is claimed. See the [public main synchronization record](milestones/PROJECT_X_N114_PUBLIC_MAIN_SYNC_2026-11-27.md), [source-tree audit](milestones/PROJECT_X_N114_SOURCE_TREE_RECONCILIATION_AUDIT_2026-11-27.md), and [Full Regression milestone](milestones/PROJECT_X_N114_V10_63_FULL_REGRESSION_WINDOWS_MSVC_2026-11-27.md).


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
