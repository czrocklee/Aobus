---
name: manage-git-flow
description: >-
  BLOCKING — Activate before git status/diff/log/add/commit/push/rebase/stash/checkout/reset/show/branch,
  or when the user says commit, push, rebase, or diff. Enforces project hygiene, scoped validation,
  proposal review, and no AI attribution in commits.
---

# Manage Git Flow

Use this skill before every Git operation in Aobus.

## Hygiene Tools

Mid-session formatting and lint are forbidden for the reasons in AGENTS.md Rule 8; this skill
only owns the commit-time gate.

Before committing, run the check-only gate:

```bash
./ao hygiene
```

`./ao hygiene` never modifies files. A failing gate blocks the commit; resolve it as follows:

- **Formatting findings:** run `./ao format` on the same scope (this is the one sanctioned
  formatting pass), report exactly which files were reformatted, and re-stage them.
- **Lint findings (clang-tidy / Ruff / mypy):** fix them manually — most C++ findings have no safe
  auto-fix — then re-run scoped validation.

Re-run `./ao hygiene` until it is clean before staging.

## Validation

Run the narrowest meaningful build or test after code changes. Use `./ao check` when there is no
safer focused check. Preserve unrelated worktree changes.

## Fleet Proposals

An `aobus-fleet` result is never landed automatically. Before staging a fleet patch:

1. Read `manifest.yaml`, `review.md` or `dossier.md`, `evidence.yaml`, and the harness `patch`.
2. Review the patch semantically and apply or modify it on the real tree.
3. Run real-tree validation.
4. Record `accept`, `modify`, or `reject` with `aobus-fleet review record`.

Route statistics affect selection and breaker state only; an oracle pass is not acceptance authority.

## Commit Procedure

1. Inspect `git status`, `git diff HEAD`, and `git log -n 3`.
2. Confirm implementation and debugging are complete.
3. Run `./ao hygiene`; resolve any findings as described in Hygiene Tools and re-run until clean,
   then run scoped validation if code changed.
4. Stage only intended changes.
5. Commit with an imperative message describing the primary technical contribution. Do not mention AI,
   internal plans, or append co-author signatures.
6. Run `git status` and report any remaining unrelated changes.

Never use destructive checkout, restore, or reset operations without explicit user approval.
