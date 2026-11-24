<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# G62R18 — Scoped validation record

**Recorded:** 24 November 2026, 17:51:46 (local host time, UTC+03:30)  
**Validation ID:** G62R18  
**Workspace:** XForm_test (local Windows workspace)  
**Source/workspace baseline:** N113-G62R16  
**Overall result:** PASS — scoped validation only

## 1. Scope

G62R18 configured and built these three CMake targets, then ran each suite with the full suite selection (filter = all):

- xfrontend_tests_n111
- xfrontend_tests_n112
- xfrontend_tests_n113

The runner also compared report IDs with the source registration manifests and retained source-hash, toolchain, configure, build, console, and per-suite report evidence.

## 2. Test results

| Suite | Discovered | Executed | Passed | Failed | Skipped | Unique IDs | Duplicate / missing / unexpected |
|---|---:|---:|---:|---:|---:|---:|---|
| N111 | 23 | 23 | 23 | 0 | 0 | 23 | 0 / 0 / 0 |
| N112 | 25 | 25 | 25 | 0 | 0 | 25 | 0 / 0 / 0 |
| N113 | 204 | 204 | 204 | 0 | 0 | 204 | 0 / 0 / 0 |
| **Total** | **252** | **252** | **252** | **0** | **0** | **252** | **0 / 0 / 0** |

All three executable exit codes were 0. All three JSON reports have summary.status = passed. All three validation-comparison files report ManifestMatches = true and Status = PASS. The reported test IDs matched the registered source IDs for each suite.

The test metadata records a project inventory total of 175,937. This number is not the executed test count. This validation ran the 252 tests in the three named targets only; the remaining project inventory was not run by G62R18.

## 3. Build environment

| Property | Recorded value |
|---|---|
| Host | Windows x64 |
| Compiler | MSVC 19.51.36248 |
| Build system | CMake 4.4.4 |
| Generator | NMake Makefiles |
| Configuration | Debug |
| Debug information | /Z7 |
| Configure exit code | 0 |
| Build exit code | 0 |
| Built targets | xfrontend_tests_n111, xfrontend_tests_n112, xfrontend_tests_n113 |

## 4. Source integrity

The runner checked the following 16 files against the expected SHA-256 values before the run and checked them again after the run. All 16 existed, matched the expected digest, and retained the same digest after the run.

| File | SHA-256 |
|---|---|
| CMakeLists.txt | DC96B81FCC86DA598998E9B344D0DE1D2C45DBAE95A3100E616F8D6216A6F9A6 |
| src/x64_codegen.cpp | AA16D6BA6B60D0E10221A5765A48BB8025E2580E4EF18845A980F4558ABBB508 |
| src/x64_location_transfer.cpp | 9A70861BBF4F981A0466E672E56AB49E03A955479768136DB889F5C79771FE8D |
| src/native_register_analysis.cpp | CED647CFD9411AC9B41C93EC998BF2A43FBD1B7526F9F36583B09AB8ABA14FE4 |
| src/native_cfg_bridge.cpp | 2DF8CC162DE91C6C5E67EC41F6B95274592B45E54F296FBE15E1A1C4B4141493 |
| src/linear_scan_allocator.cpp | 15B0FE93AFE6795F6A0DECF4F243C6E27A90280E0E3657B1A8A9DB9EF2DFBECC |
| include/xfrontend/x64_codegen.hpp | 16010E7CDCCDC47D57B64CD290A177C6EF8B33FA252FAB0AADC8A68672B3A0EE |
| include/xfrontend/x64_location_transfer.hpp | 802A6DBA2970441236D70064419DA1AC2BE59D155BB666B5007B266FCCCA1549 |
| include/xfrontend/native_register_analysis.hpp | BCCE375E12C7CA705E802186AF08B885751338C1E421CC3CB25134F26E337AF8 |
| include/xfrontend/native_cfg_bridge.hpp | A76753477D7D166307BE7D9C1B8CB6D241783F624DD551E15D8E01F80E791422 |
| include/xfrontend/linear_scan_allocator.hpp | 6B5543314FC59630A25B7A5440AA202C5210E35E10845341D3EE3FA8E5396B27 |
| tests/suite_main.cpp | EE7F90D64F3B3A28AA36942038627554A335A5B2D882A801437424D05FFC5F80 |
| tests/test_report.hpp | 4F66AE4AA595B40C8C332093D2E15BE27CE8712CFD875DFCE34BE9B58EE32032 |
| tests/n111_machine_location_plan_tests.cpp | 71450C24F0B5BA83008E9D68AEC431BBDAD9FC2184965270321D72F935B83C1C |
| tests/n112_flow_segment_plan_tests.cpp | 859C08AF8DE4853E0EA60E992A0C46C2D24F10DD8EE7E1A2E7E4F60AD25577CE |
| tests/n113_flow_spill_reload_resolution_tests.cpp | CF42B0976C8C38E6D8E2EB4A227A0B8397AA07DA31388E45CE37A5922A11F347 |

## 5. Evidence package

- Evidence package: ProjectX-G62R18-Evidence-20261124-175925-6136.zip
- ZIP SHA-256: 0E7C221B0490135CB05DD4192A1932BAA33A1552624F7AAEE97A89265796CA25
- Archive integrity: ZIP CRC test passed.
- Collection manifest: COMPLETE; 31 selected evidence files copied; 0 required files missing; 0 copy-hash mismatches; 1 optional file was absent.
- Local evidence folder: reports/G62R18-N111-N112-N113-20261124-174853-6136/
- Local build folder: build-g62r18-n111-n112-n113-msvc-z7/

The retained package includes the exact runner copy, CMake configure/build logs, MSVC and CMake version probes, CMakeCache, before/after SHA-256 CSV files, source test-ID manifests, raw JSON/TXT test reports, per-suite validation comparisons, console logs, and collection manifest.

## 6. N113 scope decision and remaining limits

G62R18 supports closing the N113 operation-level spill/reload validation scope. It does not close all N113/CFG work or establish product-wide readiness.

- G11 readiness remains 0/7.
- Edge Bundle insertion was not performed.
- CFG Edge Bundle and broader CFG integration are deferred to N114.
- Full Regression was not run.
- End-to-end execution of a generated PE was not part of this validation.

The CMake report inventory of 175,937 must not be described as 175,937 tests run.

## 7. Runner reporting note

The generated summary.txt has a formatting defect in its three per-suite display lines: literal format placeholders remain visible. The defect is limited to those summary display lines. The JSON summary, individual JSON/TXT test reports, validation-comparison files, and console logs contain the suite results and were used for this audit. The runner formatting issue should be fixed separately; it does not change the recorded test execution results.

## 8. Provenance and interpretation

The run metadata identifies the tested workspace as a local XForm_test directory. The test runner recorded GitOperations = NONE and did not record a source Git commit SHA. The evidence therefore proves the stated local-workspace run and the 16-file before/after integrity check; it does not independently prove that every file in the public repository at a named Git commit was identical to the tested workspace.

No source change, commit, push, or remote operation was made by the test run. The documentation update that references this record is a separate, documentation-only change.
