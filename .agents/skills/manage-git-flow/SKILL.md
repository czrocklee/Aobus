---
name: manage-git-flow
description: >-
  BLOCKING — Activate before any Git or GitHub workflow mutation: stage, commit, amend, create or
  switch branches, push, rebase, squash, merge, cherry-pick, stash, restore, reset, or create,
  update, or merge a pull request. Read-only status, diff, log, blame, branch, PR, and CI inspection
  does not require this skill. Enforces cohesive history, project gates, and safe publication.
---

# Manage Git Flow

Use this skill before using Git or GitHub to change the worktree, index, history, refs, remote
branches, or pull requests. Repository hooks remain authoritative.

## Invariants

- Preserve unrelated worktree changes.
- Read `AGENTS.md` and `doc/development/commit-message.md` before committing.
- Do not commit directly to the default branch unless the user explicitly requests it.
- Treat explicit history guidance as a continuing constraint until the topic changes or the user
  revises it.
- Permission to commit, push, or open a PR does not authorize merging, changing repository rules,
  or rewriting unrelated history.

## Shape History by Intent

Commits are review and revert units, not activity logs. Prefer the smallest sequence of cohesive,
independently understandable changes; do not force a branch into one commit by default.

- **Amend** when a follow-up completes or corrects the same technical intent and a separate commit
  would add no review or revert value.
- **Create a new commit** when the change is a distinct concern, deserves an independent review, or
  should be independently revertible.
- **Squash or reorder** only to restore an agreed history shape, never merely to reduce commit count.

Before rewriting a published branch, confirm the intended commit range and observed upstream tip.
Require explicit authorization unless the user already requested amend, squash, consolidation, or
equivalent history rewriting. Push with an OID-bound lease:

```bash
git push --force-with-lease=<remote-ref>:<observed-remote-oid> <remote> <local-ref>:<remote-ref>
```

Never rewrite the default branch. If the lease fails, stop and inspect the remote change; do not
weaken or retry the force push.

## Validate and Commit

1. Inspect `git status --short --branch`, `git diff HEAD`, the branch range against its base, and the
   latest commits. Check for an existing PR before deciding the history shape.
2. Confirm implementation and debugging are complete. Run the validation required by `AGENTS.md`.
3. Run the final check-only gate:

   ```bash
   ./ao hygiene
   ```

   Fix reported files deliberately. Run modifying format or lint tools only when the user has
   authorized them under `AGENTS.md`.
4. Stage only intended changes and review the staged diff.
5. Commit using `doc/development/commit-message.md`. Describe the technical result; omit AI/tool
   attribution, internal plans, co-author signatures, and validation trailers.
6. Inspect the resulting commit, worktree, and branch range before publishing.

Do not use destructive checkout, restore, or reset operations without explicit user approval.

## Publish and Pull Requests

- Use a normal push when history is additive. Use the exact lease procedure above after an
  authorized rewrite.
- Verify the remote ref resolves to the intended local commit after pushing.
- Before opening a PR, confirm the base, head, existing-PR state, and repository template. Derive the
  title and body from the full `base...HEAD` change, not only the latest commit. Never create a
  duplicate PR.
- After each push, monitor checks attached to the current HEAD SHA. Treat superseded runs as stale,
  diagnose failures before rerunning, and report any intentionally skipped platform validation.
- Merge the PR or change required checks only with separate authorization.
