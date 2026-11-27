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
| N114 v10.63 | Windows/MSVC Full Regression recorded locally (175,976/175,976; 26/26 batches; 9/9 builds). The 194-file curated source snapshot was subsequently merged into public main through PR #7; exact-commit regression provenance remains pending. |

## Current continuation point

Public `main` now contains the N114 v10.63 source snapshot. The source branch commit is `6e802faf646a301f124026d5d0661796dd97080b`, merged through PR #7 as `320c8d737aa2dc7dd9989026c6ffa3781248e77b`.

The historical Full Regression PASS remains local-workspace evidence only: the runner did not record a source commit SHA, and the seven named source guards do not prove whole-tree identity. The exact published source commit therefore still needs its own Windows/MSVC build and regression validation before that PASS can be attributed to it. See the [public main synchronization milestone](milestones/PROJECT_X_N114_PUBLIC_MAIN_SYNC_2026-11-27.md) and [Full Regression milestone](milestones/PROJECT_X_N114_V10_63_FULL_REGRESSION_WINDOWS_MSVC_2026-11-27.md).

N114 technical gates remain explicit: G9 is statement-edge-only, G10 is do-not-rebuild, G11 is 0/7, `spill_call_crossing=true`, and non-empty XMM remains open / blocked-not-proven-impossible. No Edge Bundle insertion or generated-PE end-to-end validation is claimed.

## Historical record gaps

The recovery notes identify incomplete evidence for some early N1–N10 mappings and parts of N17–N23, including N19 and Windows validation for N23. This summary leaves those gaps explicit instead of filling them with assumptions. Detailed records remain in the private working archive.
