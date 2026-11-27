<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# N114 v10.63 — source-tree reconciliation audit

**Date:** 27 November 2026  
**Reviewed public main:** `a387301b5efd7251a9346f823e2396748557743a`  
**Candidate:** `XForm_test` source archive after applying the two pinned N114 v10.63 patches  
**Status:** package prepared; source push and commit-level validation pending

## Path and content reconciliation

The public main source inventory has 193 tracked paths across CMake, examples, headers, source, tests, and tools. The curated local candidate has 194:

- All 193 existing public paths are present in the candidate.
- There are no public-main-only paths.
- One file is new: `tests/n114_b13b_expression_branch_anchor_tests.cpp`.
- **179 of the 193 existing files** were verified by Git blob SHA-1 to be byte-identical after restoring the exact SPDX notices present in public main: 172 files with a 92-byte GPL header, plus the example Persian README and six Python verifiers with path-appropriate notices.
- **14 existing files** have substantive content changes beyond the license header:
  - `CMakeLists.txt`
  - `include/xfrontend/control_flow.hpp`
  - `include/xfrontend/native_cfg_bridge.hpp`
  - `include/xfrontend/x64_codegen.hpp`
  - `src/control_flow.cpp`
  - `src/native_cfg_bridge.cpp`
  - `src/x64_codegen.cpp`
  - `src/x64_location_transfer.cpp`
  - `tests/control_flow_tests.cpp`
  - `tests/n109a_cfg_edge_anchor_tests.cpp`
  - `tests/n54_register_codegen_tests.cpp`
  - `tests/n55_n59_backend_optimization_tests.cpp`
  - `tests/n72_n77_register_codegen_tests.cpp`
  - `tests/suite_main.cpp`

Six of the 14 existing content changes correspond to N114 v10.63 final source guards (CMake, x64_codegen.hpp/cpp, and N54/N55/N72 tests). The remaining eight are carried from the broader N113/CFG source candidate and are not individually covered by the seven final guards.

## Prepared transfer package

- File: `ProjectX_N114_v10.63_SourceSyncBundle_20261127.zip`
- Size: 808,869 bytes
- SHA-256: `ADE6210D272EE09421EA858205DDFBC993E688EA730A9A71F5F75A8FBFC7EBEB`
- Contents: 194 curated source/example/test/tool files plus a complete pre/post SHA-256 and Git-blob manifest.
- Build folders, reports, caches, executables, backups, nested archives and private recovery material are excluded.
- SPDX notices are restored according to the repository's existing per-file license policy. The seven hashes recorded by the prior Windows runner were measured before this header-only restoration; published-file hashes therefore differ, while code bodies are retained.

## Source provenance boundary

The Windows/MSVC Full Regression result remains evidence for the user's local workspace: 175,976/175,976, 26/26 batches, and 9/9 build targets. The runner did not capture a source Git commit SHA. The archive-plus-patch candidate is carefully reconciled, but this does not independently prove that every unguarded file was byte-identical to the exact workspace at test time.

A dedicated source branch has been reserved: `sync/n114-v10.63-source-tree-2026-11-27`, based on the latest documentation branch head. The bundle must be applied and pushed from Windows. The exact resulting Git commit must then receive a complete tree manifest and the required build/test validation before claiming that the public repository contains the regression-tested source. A separate PR will be opened after the branch is pushed and verified.

```text
PUBLIC_MAIN_BASE=a387301b5efd7251a9346f823e2396748557743a
PUBLIC_TRACKED_SOURCE_PATHS=193
LOCAL_CANDIDATE_SOURCE_PATHS=194
EXISTING_PATHS_PRESENT=193
PUBLIC_ONLY_PATHS=0
HEADER_ONLY_GIT_BLOB_MATCHES=179
SUBSTANTIVE_EXISTING_SOURCE_CHANGES=14
N114_NEW_TEST_FILES=1
SOURCE_BUNDLE_FILES=194
SOURCE_BUNDLE_SIZE=808869
SOURCE_BUNDLE_SHA256=ADE6210D272EE09421EA858205DDFBC993E688EA730A9A71F5F75A8FBFC7EBEB
SOURCE_SYNC_BRANCH=sync/n114-v10.63-source-tree-2026-11-27
SOURCE_SYNC_BRANCH_STATUS=RESERVED_AWAITING_PUSH
SOURCE_SYNC_PR=NOT_CREATED
PUBLIC_SOURCE_SYNC=NOT_ESTABLISHED
```


## Publication follow-up — 27 November 2026

After this audit was first recorded, the source bundle was applied to and pushed on `sync/n114-v10.63-source-tree-2026-11-27` at commit `6e802faf646a301f124026d5d0661796dd97080b`. The dedicated source PR #7 was merged into public `main`; the resulting source merge commit is `320c8d737aa2dc7dd9989026c6ffa3781248e77b`. The documentation PR #6 was merged first at `0bf01d27afb0cd7df35b1506096a0524db15654d`.

The remote source PR changed exactly 15 paths: 14 existing source files and the new N114 B13-B test. The 194-file bundle hash, ZIP structure, all 194 postimage file hashes and staged Git blob IDs, and the exact staged path delta passed the local pre-commit guards; the pushed branch SHA read-back matched the local commit. The source snapshot is therefore integrated into public `main`.

This publication does not repair the missing test-run commit provenance. The historical Windows/MSVC Full Regression runner did not record a source SHA, so validation against the exact published source commit remains pending. See [the public main synchronization milestone](PROJECT_X_N114_PUBLIC_MAIN_SYNC_2026-11-27.md). No release certification is claimed.
