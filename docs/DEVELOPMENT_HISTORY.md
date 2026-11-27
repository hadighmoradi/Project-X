<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Development history

This is a public summary of the milestones recorded in the project archives. It is not a substitute for the per-patch manifests and host-validation logs.

| Milestone range | Recorded focus |
|---|---|
| N1–N10 | Initial native frontend foundations. |
| N11–N16 | Regression expansion and ownership analysis. |
| N17–N23 | Drop behavior, ABI, symbols, and binary-object work. |
| N24–N30 | Linker development and PE32+ output. |
| N31–N38 | Archives, imports/exports, DLLs, TLS, and exception/unwind support. |
| N39–N48 | Advanced PE64 work, debug support, and the start of direct code generation. |
| N49–N65 | Native AMD64 code generation, optimization, and register allocation. |
| N66–N89 | Register analysis, control-flow graphs, and multi-block code generation. |
| N90–N101 | Flow values, stack homes, memory phi handling, and persistent GP/XMM register code generation. |
| N102–N107 / v1.48.0 | CFG-aware allocation and unwind hardening. |
| N113-G1–G33 | FunctionEmitter edge/operand evidence, diagnostics, and readiness gates. The recorded gates keep general edge insertion disabled. |
| N113-G57–G62R16 | Readiness and bounded probe analysis, source cross-checks, and a 14-file source snapshot. |
| N113-G62R18 | Scoped MSVC validation of N111/N112/N113: 252/252 passed; operation-level spill/reload scope closed; CFG Edge Bundle work deferred to N114. |
| N114 v10.63 | Windows/MSVC Full Regression: 175,976/175,976, 26/26 batches and 9/9 builds; public source-tree synchronization remains pending. |

## Current continuation point

N114 v10.63 has a recorded Windows/MSVC Full Regression PASS for the declared 175,976-test inventory: 26/26 batches, zero failures, zero duplicate IDs, and a 9/9 target build matrix. The seven named source guards matched their final SHA-256 values.

This validation came from the user's local workspace; the runner did not record a source commit SHA. The local tested source tree has not yet been proven identical to public `main`. The documentation records the local result but does not synchronize compiler source. Complete public source-tree reconciliation is the next publication boundary. See the [N114 Full Regression milestone](milestones/PROJECT_X_N114_V10_63_FULL_REGRESSION_WINDOWS_MSVC_2026-11-27.md).

N114 technical gates remain explicit: G9 is statement-edge-only, G10 is do-not-rebuild, G11 is 0/7, `spill_call_crossing=true`, and non-empty XMM remains open / blocked-not-proven-impossible. No Edge Bundle insertion or generated-PE end-to-end validation is claimed.

## Historical record gaps

The recovery notes identify incomplete evidence for some early N1–N10 mappings and parts of N17–N23, including N19 and Windows validation for N23. This summary leaves those gaps explicit instead of filling them with assumptions. Detailed records remain in the private working archive.
