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

## Current continuation point

The latest checkpoint is N113-G62R16 (23 November 2026). G62R17 is a proposed analysis step: compare expression-temporary liveness and register ownership with N112 Flow locations before designing opt-in routing. It has not been performed.

## Historical record gaps

The recovery notes identify incomplete evidence for some early N1–N10 mappings and parts of N17–N23, including N19 and Windows validation for N23. This summary leaves those gaps explicit instead of filling them with assumptions. Detailed records remain in the private working archive.
