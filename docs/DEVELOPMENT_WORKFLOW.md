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

When a step is finished and checked, update the relevant project notes:

- docs/STATUS.md for the latest status and next step.
- docs/DEVELOPMENT_HISTORY.md for completed milestones.
- docs/ARCHITECTURE.md or the language design pages when a design changes.
- The test notes and README when the instructions or public claims change.

Update the English and Persian pages together whenever possible.

## Publish the result

For each completed step, publish the change to GitHub with a clear commit message. For larger changes, use a focused branch and pull request. Link the related tests and notes so that someone else can follow the result.

Keep old test results and project history. Do not replace a past result with a new one; add the new result and explain what changed.

## The main rule

A development step is complete when the work has been checked, its results and limits have been recorded, the related project pages have been updated, and the change has been published on GitHub.
