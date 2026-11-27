<!-- SPDX-FileCopyrightText: 2026 Hector Moradi -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Project X — Continuous GitHub Synchronization Policy

**Adopted:** 27 November 2026  
**Authority:** The user's standing instruction on 27 November 2026.

## Principle

GitHub must remain current with every Project X change that is suitable for public engineering records. After each meaningful design, source, build, test, runner, validation, audit, or execution step, assess whether its result can be published. When it can, recording it in the repository and updating the relevant public pages is part of completing the work—not an optional follow-up requiring another reminder.

## Standing authorization

The user has granted continuing authorization for focused branch creation, commits, pushes, and Pull Request creation/updates for eligible Project X changes. Do not repeatedly ask permission for those branch-scoped actions.

This authorization does **not** authorize merging to `main`, direct writes to `main`, release/tag publication, destructive file/branch operations, force-push/history rewrite, security/permission changes, or publication of confidential/sensitive data. Those require separate explicit approval.

Default workflow: **Branch → Commit → Push → Pull Request; no Merge.**

## What to publish

Publish appropriate results from builds, tests, regression batches, marker/ID audits, source hash guards, runner/validator corrections, toolchain changes, technical decisions, limitations, and relevant failures. Preserve negative or inconclusive results with their real status; never hide failures or promote them to PASS.

Keep confidential, sensitive, temporary, or inadequately verified material out of public pages. Record the reason in the private Master when useful.

## Pages to synchronize

Update the relevant English and Persian pages together: README, STATUS, development history, getting-started/build instructions, language design, development workflow, and website pages such as `index.html`, `language-design.html`, and `project-guide.html`. Add a dated milestone under `docs/milestones/` for substantial results. Preserve historical records rather than overwriting them. Verify internal links and relative paths.

## Evidence requirements

Public records should state the date, exact scope, environment/toolchain, commands or runner identity, expected/executed/passed/failed/skipped counts, ID/duplicate checks, source revision or explicit lack of commit provenance, relevant hashes, and limitations.

A local-workspace result is evidence for that workspace only. Do not claim that public `main` contains the exact tested source unless the commit SHA and complete source-tree identity are established. A limited hash guard is not a full-tree hash. Distinguish build, test, patch application, source audit, and end-to-end outcomes. Do not describe inventory metadata as tests executed or a scoped PASS as release certification.

## Required post-step workflow

1. Assess publishability, provenance, scope, and sensitivity.
2. Inspect `main` and existing PRs; avoid duplicate branches/PRs.
3. Create a unique focused branch.
4. Update all relevant bilingual docs and website pages; add a milestone where warranted.
5. Commit and push the branch.
6. Read back and verify branch head, changed files, base/head SHAs, links, and PR state.
7. Keep a PR Draft while provenance or required validation remains pending.
8. Record the verified PR and its limits in the Master and user report.
9. Never merge automatically.

If the GitHub connector cannot transfer a complete source tree, state the blocker, complete supported documentation work, and provide a verified package plus a complete Windows PowerShell 5.1 script where appropriate. Never claim GitHub is synchronized until the branch/commit/PR has actually been verified.

## Current record — 27 November 2026

- Repository: `hadighmoradi/Project-X`.
- Documentation PR #6: https://github.com/hadighmoradi/Project-X/pull/6 — Open, Draft, not merged.
- Branch: `docs/n114-full-regression-record-2026-11-27`.
- Head commit at the previous documentation checkpoint: `9859f0920c92670d14ef1ea13d2fa40bde987b7a`.
- PR #6 records local N114 v10.63 Full Regression: 175,976/175,976, 26/26 batches, and 9/9 build targets.
- Complete source-tree synchronization with public `main` remains unestablished; PR #6 is documentation-only.

```text
GITHUB_CONTINUOUS_SYNC=REQUIRED_WHEN_PUBLISHABLE
USER_STANDING_AUTHORIZATION_DATE=2026-11-27
BRANCH_COMMIT_PUSH_PR=AUTHORIZED_WITHOUT_REPEATED_PROMPT
DIRECT_MAIN_WRITE=NOT_AUTHORIZED
PR_MERGE=REQUIRES_SEPARATE_EXPLICIT_APPROVAL
RELEASE_OR_TAG=REQUIRES_SEPARATE_EXPLICIT_APPROVAL
BILINGUAL_DOCS_AND_SITE_SYNC=REQUIRED_WHEN_RELEVANT
EVIDENCE_PROVENANCE=REQUIRED
SOURCE_TREE_IDENTITY=DO_NOT_ASSUME
PUBLIC_SOURCE_SYNC=NOT_ESTABLISHED
```


## Publication update — 27 November 2026

The repository owner explicitly authorized the merge of the two named PRs in the current conversation. They were merged through the PR workflow, not by direct writes to `main`:

- PR #6 documentation/site checkpoint — merged; merge commit `0bf01d27afb0cd7df35b1506096a0524db15654d`.
- PR #7 N114 v10.63 source snapshot — merged; source commit `6e802faf646a301f124026d5d0661796dd97080b`, merge commit `320c8d737aa2dc7dd9989026c6ffa3781248e77b`.
- Public `main` now contains the 15-path source delta from the curated 194-file candidate.
- Exact-commit Windows/MSVC validation remains pending because the historical runner recorded no source commit SHA. The queried PR heads had no GitHub status checks or workflow runs. No release/tag or release-certification claim was made.

The default remains Branch → Commit → Push → Pull Request. Merge requires specific, separate user approval; the approval recorded here was limited to PRs #6 and #7.

```text
PUBLIC_SOURCE_SYNC=INTEGRATED_IN_MAIN
SOURCE_COMMIT=6e802faf646a301f124026d5d0661796dd97080b
SOURCE_MERGE_COMMIT=320c8d737aa2dc7dd9989026c6ffa3781248e77b
DOCS_MERGE_COMMIT=0bf01d27afb0cd7df35b1506096a0524db15654d
PR6=MERGED
PR7=MERGED
EXACT_SOURCE_COMMIT_REGRESSION=PENDING_PROVENANCE_AND_RERUN
GITHUB_CI_CHECKS=NONE_RETURNED
RELEASE_CERTIFICATION=NOT_CLAIMED
```
