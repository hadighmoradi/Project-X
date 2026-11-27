<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# N114 — Expression-level control flow for short-circuit operators

**Recorded:** 27 November 2026

## Why this work matters

Logical expressions such as `left && right` and `left || right` do not always evaluate both operands. The right-hand expression may be skipped depending on the value of the left-hand expression. The compiler must model that behavior correctly during analysis and preserve the corresponding paths when generating native code.

## What changed

- Logical expression nodes receive stable identities within the function's control-flow model.
- The control-flow graph represents the paths for evaluating the right-hand side, taking the short-circuit path, and joining the results.
- Analysis reachability is kept separate from the need to represent a path for machine-code generation. A path that cannot execute for a known constant condition can be excluded from analysis adjacency without losing its expression identity in the compiler's emission model.
- The x64 backend records expression branch anchors and validates their source, target, and instruction locations.
- Branch locations are tracked as code layout changes, and register-state observations are retained at the relevant expression-emission boundaries.

## Result in the compiler

The compiler has a more explicit representation of short-circuit expression control flow. This improves the separation between semantic/control-flow analysis and native branch emission, and gives the backend concrete information for checking branch destinations and code offsets.

This is compiler-internal progress. It does not introduce a new surface syntax: `&&` and `||` are the logical operators being represented more precisely. It also does not mean that the complete language, runtime, standard library, IDE, visual form designer, Linux native compiler path, or x86-32 backend is complete.

## Current platform scope

The integrated native code-generation path is Windows x64: the compiler emits AMD64 COFF object modules and a deterministic link plan for the platform linker. Linux ELF64 components exist, but a complete source-to-ELF compiler path remains in development. x86-32 and macOS output remain future targets.

For the broader language direction, see [language design](../LANGUAGE_DESIGN.md), [architecture](../ARCHITECTURE.md), and [current status](../STATUS.md).
