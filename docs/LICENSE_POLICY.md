<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Project X licensing policy

This page describes the current repository policy and separates it from plans for components that have not yet been published.

## License by component

| Component | Current policy |
|---|---|
| Compiler, backend, project libraries, tests, development tools, build scripts, and project-owned source fixtures | GPL-3.0-or-later |
| Project documentation and website content | CC BY-SA 4.0 |
| Standalone examples | GPL-3.0-or-later by default; MIT only when the individual file explicitly declares it |
| Runtime library | Planned: LGPL-3.0-or-later with the X Runtime Linking Exception; not included in this snapshot |
| Third-party components | Their own applicable licenses and notices |

## What is included now?

The root [LICENSE](../LICENSE) contains the standard GNU GPL version 3 license text. Project-owned code files should identify the intended **GPL-3.0-or-later** grant with SPDX notices. The documentation license text is in [LICENSES/CC-BY-SA-4.0.txt](../LICENSES/CC-BY-SA-4.0.txt). Component scope is summarized in [COPYRIGHT.md](../COPYRIGHT.md).

The runtime policy is only a plan at this stage. This source snapshot contains no runtime implementation and no finalized text for the X Runtime Linking Exception. The exception must be drafted and reviewed before a runtime is distributed. It must clearly define permissions and conditions for static and dynamic linking, relinking, runtime modification, and distribution. Until then, no file in this snapshot should be treated as receiving that exception.

## Programs compiled with Project X

The compiler's GPL license does not automatically make every independently written program compiled with it a GPL work. The license of an output depends on what it contains and whether it incorporates or is derived from copyrighted project material, such as runtime code or templates. Review bundled components and generated material separately.

## Contributions

Contributors retain copyright in their own contributions. By submitting a contribution, they must have the right to provide it under the license applicable to that component: GPL-3.0-or-later for code and CC BY-SA 4.0 for project documentation. The current project policy does not require copyright assignment or a CLA. This does not authorize relicensing another contributor's work under different terms.

Contributions to a future runtime or the planned linking exception need a separate, explicit policy before they are accepted under those terms. See [CONTRIBUTING.md](../CONTRIBUTING.md).

## Third-party materials and release checks

Third-party source, binary, generated, or copied material must retain its original notices and compatible license terms. The project still needs a complete dependency inventory, license-compatibility review, and notice audit before a release. The existence of this policy page does not mean that audit has passed.

This page is a project policy summary, not legal advice.
