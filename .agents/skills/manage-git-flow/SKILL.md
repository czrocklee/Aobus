---
name: manage-git-flow
description: >-
  BLOCKING — Activate before git status/diff/log/add/commit/push/rebase/stash/checkout/reset/show/branch,
  or when the user says commit, push, rebase, or diff. Enforces targeted formatting, scoped validation,
  proposal review, and no AI attribution in commits.
---

# Manage Git Flow

Use this skill before every Git operation in Aobus.

## Formatting

Do not run `clang-format` during ordinary implementation or validation. Run one targeted pass only when
the user explicitly requests formatting or immediately before a commit:

```bash
while IFS= read -r -d '' entry; do
  st="${entry:0:2}"
  f="${entry:3}"
  if [[ "$st" =~ ^[RC] ]]; then IFS= read -r -d '' f || break; fi
  if [ -f "$f" ] && { [[ "$f" == *.cpp ]] || [[ "$f" == *.h ]] || [[ "$f" == *.hpp ]]; }; then
    clang-format -i "$f"
    printf 'formatted %s\n' "$f"
  fi
done < <(git status --porcelain -z)
```

Report exactly which files were formatted. Do not run clang-tidy unless the user explicitly requested
lint work.

## Validation

Run the narrowest meaningful build or test after code changes. Use `./build.sh debug` when there is no
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
3. Run the targeted formatting loop once and report its output.
4. Re-run scoped validation after formatting.
5. Stage only intended changes.
6. Commit with an imperative message describing the primary technical contribution. Do not mention AI,
   internal plans, or append co-author signatures.
7. Run `git status` and report any remaining unrelated changes.

Never use destructive checkout, restore, or reset operations without explicit user approval.
