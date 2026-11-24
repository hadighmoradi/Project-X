<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Project X development and publication workflow

## Core rule

**Every engineering phase or checkpoint that is declared complete must be documented, verified, and published in the GitHub repository.** The repository's Markdown files and public project information must be updated as part of completing that phase, not left for an unspecified later cleanup.

## Completion checklist

Before marking a phase complete:

1. **Identify the exact state.** Record the source revision/commit being tested, the relevant checkpoint or patch ID, and the scope of the change.
2. **Record the environment.** Include operating system, compiler/toolchain and version, build configuration, relevant hardware when performance is measured, and important dependencies.
3. **Record what was actually run.** List commands, test suites, selected cases, counts, and artifact/log locations. Distinguish tests that passed, failed, were skipped, or were not run.
4. **Record the evidence and boundaries.** Summarize the result, known limitations, unresolved issues, deviations, and the next concrete step. Do not convert a limited probe into a general support claim.
5. **Update the documentation.** Update the relevant files, normally including:
   - `docs/STATUS.md` for the latest verified checkpoint and next step;
   - `docs/DEVELOPMENT_HISTORY.md` for completed milestone history;
   - `docs/ARCHITECTURE.md` or the relevant language/design document when architecture or language decisions change;
   - the relevant test/validation matrix and README when public claims or instructions change.
6. **Publish the change.** Commit implementation, tests, reports, and documentation together where practical. Use a focused branch and pull request for substantial changes, and link the PR to the exact checkpoint and evidence.
7. **Keep public information consistent.** Update the English and Persian project pages/README files when a public status, capability, roadmap, or decision changes.

## Required status labels

Use these labels consistently:

- **Proposed:** an idea under discussion; not approved.
- **Designed:** a design is documented; implementation may not exist.
- **Prototype:** an experimental or isolated implementation exists; production integration is not established.
- **Implemented:** the feature exists in the project source.
- **Verified:** the named implementation passed the specified tests on the recorded environment.
- **Not ready / blocked:** a required gate has not passed or a dependency remains unresolved.
- **Not run:** the test or validation was not performed. Never imply success.

A feature should not be described as generally supported merely because its syntax appears in a design document or a prototype handles one controlled case.

## Evidence and traceability

A completion record should make it possible for another developer to answer:

- Which exact source revision was examined?
- What changed in this phase?
- Which build and test commands were run?
- What passed, failed, or was not run?
- What are the known limitations and remaining blockers?
- Which GitHub commit, pull request, report, and Markdown pages describe the result?

Reference the tested source revision and the publication commit/PR. Do not try to write a commit hash into the same commit that is creating it; link the resulting GitHub commit or PR in the phase record or subsequent status update.

## Design and engineering decisions

- Keep product goals, design proposals, approved decisions, prototypes, and verified implementation separate.
- When a user decision is required, document the options and wait for explicit approval; do not silently convert an open proposal into a decision.
- When a design decision is approved, update its design document and the public summary if relevant.
- When implementation changes, update code, tests, and relevant documentation together.
- If full regression is not run, state that clearly, even when focused tests pass.

## Repository safety and history

- Preserve historical records, manifests, test logs, and recovery archives. Do not silently delete or overwrite them.
- Prefer additive, reviewable changes. Avoid unrelated formatting or broad cleanup in a functional patch.
- Use a branch and pull request for substantial work. Do not merge a new PR into `main` until the repository owner has approved it, unless the owner explicitly instructs otherwise.
- Keep the local workspace and GitHub repository synchronized through an identifiable patch/commit. A GitHub update does not mean the user's local Windows workspace was modified.

## Phase completion means publication

A phase is complete only when its technical acceptance criteria are met, evidence is recorded, relevant Markdown and public summaries are updated, and the resulting change is committed and published to GitHub. If any of these steps is still pending, report the phase as pending publication rather than complete.