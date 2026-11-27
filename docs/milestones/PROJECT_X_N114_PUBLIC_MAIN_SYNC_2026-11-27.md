<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# N114 v10.63 — public main source synchronization

**Recorded:** 27 November 2026  
**Repository:** `hadighmoradi/Project-X`  
**Default branch:** `main`

## Publication result

The N114 v10.63 source snapshot is now integrated into public `main`.

| Item | Verified value |
|---|---|
| Documentation PR | [#6](https://github.com/hadighmoradi/Project-X/pull/6) — merged |
| Source PR | [#7](https://github.com/hadighmoradi/Project-X/pull/7) — merged |
| Source branch commit | `6e802faf646a301f124026d5d0661796dd97080b` |
| Source merge commit on main | `320c8d737aa2dc7dd9989026c6ffa3781248e77b` |
| Documentation merge commit | `0bf01d27afb0cd7df35b1506096a0524db15654d` |
| Source PR changed paths | 15 (14 modified, 1 new) |
| Curated source manifest | 194 files |
| Bundle SHA-256 | `ADE6210D272EE09421EA858205DDFBC993E688EA730A9A71F5F75A8FBFC7EBEB` |

The 194-file source candidate comprised all 193 existing tracked source paths plus the new `tests/n114_b13b_expression_branch_anchor_tests.cpp`. The audit identified 179 existing files matching the base Git blob identities after license-header restoration and 14 existing files with substantive changes. The source PR therefore changed exactly 15 paths.

Before the commit was made, the bundle hash, ZIP structure, all 194 file SHA-256 values, all 194 staged Git blob IDs, and the exact 15-path staged diff were checked. The source commit was pushed, and the remote branch read-back matched its local commit SHA. GitHub reported PR #7 merged successfully.

## Validation provenance — important limitation

The reported Windows/MSVC Full Regression remains a genuine local-workspace result: 175,976/175,976 across 26/26 batches, with 9/9 build-matrix targets and seven named source hash guards reporting zero failures.

The original regression runner did **not** record a source commit SHA. Therefore, the historical PASS is **not** evidence that the complete test run was executed against source commit `6e802faf646a301f124026d5d0661796dd97080b` or its merge commit `320c8d737aa2dc7dd9989026c6ffa3781248e77b`. The seven guards do not prove whole-tree identity. Rebuild and rerun the required Windows/MSVC validation against the exact published source revision before attributing that historical PASS to it.

The GitHub checks queried for the two PR heads returned no status checks or associated workflow runs. This is not a passing CI result.

## Scope retained

This synchronization does not change the recorded technical gates: G9 remains `STATEMENT_EDGE_ONLY`, G10 remains `DO_NOT_REBUILD`, G11 remains `0/7`, `spill_call_crossing=true`, and non-empty XMM remains `OPEN/BLOCKED_NOT_PROVEN_IMPOSSIBLE`. It does not establish Edge Bundle insertion, SpillSlot reservation, unwind/relocation changes, language/type/cast/conversion changes, generated-PE end-to-end execution, or release certification.

No Release or tag was created.
