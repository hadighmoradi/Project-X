<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Project X — N114 v10.63 Windows/MSVC Full Regression PASS

**Recorded:** 27 November 2026  
**Status:** `PASS_FULL_REGRESSION`  
**Evidence origin:** User-run Windows PowerShell 5.1 build and regression reports, reconciled read-only

## Result

| Measure | Result |
|---|---:|
| Declared inventory | **175,976** |
| Valid regression batches | **26 / 26** |
| Reported passed / failed | **175,976 / 0** |
| Globally unique test IDs | **175,976** |
| Internal / cross-batch duplicates | **0 / 0** |
| Windows build matrix | **9 / 9 targets, exit code 0** |
| Source hash guards | **7 / 7, zero failures** |

This covers the full declared inventory in the 26-batch plan, not tests outside it, every ABI/configuration variant, every compiler feature, or release certification.

## Batch totals

The main inventory contains 175,009 tests across 18 batches; eight late suites add 967.

| Batch | Passed | Batch | Passed |
|---|---:|---|---:|
| M01_AF | 538 | M10_N7 | 20,000 |
| M02_GM | 142 | M11_N8A | 12,000 |
| M03_OT | 1,158 | M12_N8B | 8,000 |
| M04_UZ | 600 | M13_N90_N94 | 10,000 |
| M05_N0_N2 | 32,562 | M14_N95 | 2,000 |
| M06_N3 | 20,009 | M15_N96 | 2,000 |
| M07_N4 | 20,000 | M16_N97 | 2,000 |
| M08_N5 | 20,000 | M17_N98 | 2,000 |
| M09_N6 | 20,000 | M18_N99 | 2,000 |

| Late batch | Passed | Late batch | Passed |
|---|---:|---|---:|
| L19_N102_N107 | 601 | L23_N111 | 23 |
| L20_N108 | 23 | L24_N112 | 25 |
| L21_N109A | 34 | L25_N113 | 204 |
| L22_N109B_N110A | 37 | L26_N114_B13B | 20 |

All 26 Attempt01 reports had a successful stage exit, TXT and JSON reports, and a validated PASS marker.

## Windows build matrix

All nine targets exited 0; NMake was serial: `xfrontend_tests`, `xfrontend_tests_n102_n107`, `xfrontend_tests_n108`, `xfrontend_tests_n109a`, `xfrontend_tests_n109b_n110a`, `xfrontend_tests_n111`, `xfrontend_tests_n112`, `xfrontend_tests_n113`, and `xfrontend_tests_n114_b13b`.

## Regression compatibility changes

The Full Regression Enablement patch:
- corrected two legacy `NativeRegisterAllocationOptions` initializers in N54 to explicitly retain false defaults;
- updated N56D's frame-size expectation to account for local/spill slots, callee-save GP/XMM storage, and Win64 alignment;
- aligned N73A with the N96 persistent-register architecture; and
- corrected `XFRONTEND_TEST_INVENTORY_TOTAL` from 175,969 to 175,976 without changing target/source lists.

A Validator false-negative initially rejected 18 main batches because filtered runs report out-of-batch tests as skipped. The correct check is `EXPECTED_SKIPPED = 175009 - EXPECTED_BATCH_COUNT`; all 18 matched. This was a read-only reconciliation, not a test rerun or report rewrite.

## Seven final source hash guards

| File | SHA-256 |
|---|---|
| `include/xfrontend/x64_codegen.hpp` | `C6FA4EC251FEA12C2D5D3FFCA97FC13D79303E854AB7B47FB014EACF15F646D0` |
| `src/x64_codegen.cpp` | `9015328B635C30FFEBD7AB7343C7197531E0245267F8A744547D5FF31CBFC3A8` |
| `tests/n114_b13b_expression_branch_anchor_tests.cpp` | `8B8F8AC665651AD76FFC5ED6655E1236303DFD1E72889C6BF55443809C4233AD` |
| `CMakeLists.txt` | `775854D8769DDD612477B5FF137E4B9237C36AC00B577FADEEBFC3AC06E8BC44` |
| `tests/n54_register_codegen_tests.cpp` | `749A4A85A4010EADD1685757E5CBCFB2D75BEE9DC231CE943EBC417B4BB5D672` |
| `tests/n55_n59_backend_optimization_tests.cpp` | `77096BB5419E47A6D29C4A7D0EE2F75DC953AD330ADEAA3CA0F38645ABDE3140` |
| `tests/n72_n77_register_codegen_tests.cpp` | `8D60C46B7C80D08ABB23AF9CBB46C59716D30D0CC27DCC8C554E7087A32E64F6` |

## Technical boundaries

`G9=STATEMENT_EDGE_ONLY`; `G10=DO_NOT_REBUILD`; `G11=0/7`; `spill_call_crossing=true`; XMM non-empty remains `OPEN/BLOCKED_NOT_PROVEN_IMPOSSIBLE`. No Edge Bundle insertion, SpillSlot reservation, unwind/relocation or language/type/cast/conversion change, or generated-PE end-to-end execution is established. This is not a release certification.

## Provenance and public source boundary

The reports were produced by the user in a local Windows/MSVC workspace. The runner did not record a source Git commit SHA. The seven guards cover seven named files, not the entire workspace tree.

The public `main` revision reviewed during this documentation update was `a387301b5efd7251a9346f823e2396748557743a`. Read-only comparison found source-tree differences: the N114 B13-B test file is absent from that public tree, and several core source files have different sizes. This document records a valid local-workspace Full Regression PASS; it does not prove that the then-current public `main` contained the exact tested source.

This documentation-only change deliberately does not import an unverified subset of source files. Complete source-tree reconciliation and synchronization must be handled separately before the public repository can claim to contain this regression-tested source.

```text
PROJECT_X_TEST_INVENTORY=175976
EXPECTED_BATCHES=26
VALIDATED_BATCHES=26
TOTAL_PASS=175976
TOTAL_FAIL=0
GLOBAL_UNIQUE_IDS=175976
GLOBAL_DUPLICATES=0
SOURCE_HASH_GUARD_FAILURES=0
WINDOWS_BUILD_MATRIX=PASS_9_OF_9
FULL_REGRESSION=PASS_FULL_REGRESSION
PUBLIC_MAIN_SOURCE_SYNC=NOT_ESTABLISHED
G9=STATEMENT_EDGE_ONLY
G10=DO_NOT_REBUILD
G11=0/7
spill_call_crossing=true
XMM_NONEMPTY=OPEN_BLOCKED_NOT_PROVEN_IMPOSSIBLE
```
