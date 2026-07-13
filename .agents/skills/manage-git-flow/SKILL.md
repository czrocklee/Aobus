---
name: manage-git-flow
description: >-
  BLOCKING — Activate before any git operation that mutates repository state
  (add/commit/push/rebase/merge/cherry-pick/stash/checkout/restore/reset), or when the user says
  commit, push, or rebase. Read-only inspection (status/diff/log/show/blame/branch listing) does
  not require this skill. Enforces project hygiene, validation, and no AI attribution in commits.
---

# Manage Git Flow

Use this skill before any Git operation that mutates repository state. Read-only inspection
commands (`git status`, `git diff`, `git log`, `git show`, `git blame`) may run without it;
the repository's own git hooks still enforce hard rules such as the no-AI-attribution check.

## Hygiene Tools

Mid-session formatting and lint are forbidden for the reasons in AGENTS.md Rule 8; this skill
only owns the commit-time gate.

Before committing, run the check-only gate:

```bash
./ao hygiene
```

`./ao hygiene` never modifies files. A failing gate blocks the commit; resolve it in this order:

1. **Formatting first.** Run `./ao format` on the same scope (the one sanctioned formatting pass —
   cheap, no compile), report exactly which files were reformatted, and re-stage them. Do this
   before touching lint: formatting shifts line numbers, so fixing lint first strands the tidy
   findings on stale lines and forces an extra, expensive clang-tidy pass.
2. **Lint findings (clang-tidy / Ruff / mypy).** Fix them manually against the post-format lines —
   most C++ findings have no safe auto-fix — then re-run scoped validation.

Re-run `./ao hygiene` to verify before staging. Done in this order, clang-tidy runs only twice
(discover + verify).

## Validation

For commit requests, use the final project gate directly:

```bash
./ao check
```

The gate is fast enough that fine-grained pre-commit test selection is usually wasted agent loop
time and can miss coverage from tooling, lint integration, or platform suites. Use narrower tests
only while actively debugging a failure or validating a small hypothesis before the final gate.
Preserve unrelated worktree changes.

## Commit Procedure

1. Inspect `git status`, `git diff HEAD`, and `git log -n 3`.
2. Confirm implementation and debugging are complete.
3. Run `./ao hygiene`; resolve any findings in the order given in Hygiene Tools (format first, then
   lint) and re-run until clean.
4. Run `./ao check`; fix any failures and re-run it until clean.
5. Stage only intended changes.
6. Commit using `doc/development/commit-message.md`. The subject must describe the primary technical
   contribution and must not mention AI, internal plans, or append co-author signatures.
   Do not add validation trailers such as `Validation: ./ao check`; report validation results in the
   final user-facing response instead.
7. Run `git status` and report any remaining unrelated changes.

Never use destructive checkout, restore, or reset operations without explicit user approval.
