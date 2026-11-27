<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# How we develop Project X

We want the project to be easy to follow and its progress easy to check. When a development step is finished, the code, tests, notes, and public project information should all tell the same story.

## Before calling a step complete

- Record the exact source revision and the part of the project that changed.
- Note the operating system, compiler, tools, and build settings used.
- List the commands and tests that were actually run.
- Report what passed, what failed, and what was not tested.
- Note known limits and the next step.

## Keep the status clear

Use these descriptions in project notes:

- **Planned:** something we would like to build.
- **Designed:** the idea and its rules have been written down.
- **Prototype:** an early version exists, but it is not ready for general use.
- **Implemented:** the feature is present in the source code.
- **Verified:** the stated tests passed in the stated environment.
- **Blocked:** a required part is not ready.
- **Not tested:** no test has been run for that item.

A code example in a design document is not proof that the compiler supports it. A successful test of one small case is not proof that every case works.

## Keep the project pages up to date

After each meaningful build, test, regression, runner/validator change, source audit, or engineering decision, assess whether the result is suitable for public publication. When it is, update the relevant pages as part of that same work:

- `README.md` and `README_FA.md` for the public project summary.
- `docs/STATUS.md` and `docs/STATUS_FA.md` for current engineering status.
- Development history pages for completed milestones.
- Getting-started/build and language-design pages when their instructions or claims change.
- Website pages when they display a changed status, result, or instruction.

Update English and Persian pages together. Add a dated milestone for substantial test or validation results, and preserve historical records instead of overwriting them.

## Publish the result

The user has granted continuing authorization for focused branch creation, commits, pushes, and Pull Request creation/updates for suitable Project X changes. Do not ask for repeated permission for those branch-scoped actions.

Use the default workflow: **Branch → Commit → Push → Pull Request; no Merge.** Inspect `main` and existing PRs first, avoid duplicate work, use clear commit messages, and read back the resulting branch/commit/PR to verify the publication.

Merging to `main`, direct writes to `main`, releases/tags, destructive file or branch changes, force-push/history rewrites, and security/permission changes still require separate explicit approval.

If source provenance, CI, or required validation is incomplete, keep the PR in Draft and state the limit. A local-workspace test result must not be represented as proof that public `main` contains the exact tested source unless commit SHA and complete source-tree identity are established.

See the [continuous GitHub synchronization policy](operations/GITHUB_CONTINUOUS_SYNC_POLICY.md) for the full workflow.

## The main rule

A development step is complete when the work has been checked, its results and limits have been recorded, the related project pages have been updated, and the suitable change has been published on a reviewed GitHub branch and Pull Request. Do not merge automatically.
