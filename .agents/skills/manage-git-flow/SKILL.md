---
name: manage-git-flow
description: Manage Aobus commits, branches, and pull requests. Use before Git or PR mutations; read-only inspection does not require this skill.
---

# Manage Aobus Git flow

Preserve unrelated changes and use a topic branch unless the user requested
otherwise. Repository hooks remain authoritative. Read
`doc/development/commit-message.md` before committing.

## History and authorization

Use commits as coherent review/revert units. Amend follow-ups that complete the
same intent when unpublished or when rewriting is authorized; otherwise use an
additive commit. Separate independently useful changes. Preserve the user's chosen
history shape across turns. Do not squash merely to reduce commit count.

Before rewriting published history, establish the intended range and observed
remote tip. Rewriting requires explicit authorization; an existing request to
amend or squash supplies it. Never rewrite the default branch. Push with an
OID-bound lease:

```bash
git push --force-with-lease=<remote-ref>:<observed-remote-oid> <remote> <local-ref>:<remote-ref>
```

If the lease fails, inspect the remote change; do not weaken the lease.
Destructive checkout, restore, or reset operations need explicit approval.
Commit/push/PR authorization does not authorize merging or changing repository rules.

## Commit and publish

Inspect the worktree, staged diff, branch range, and existing PR. Stage only the
intended changes. Use `doc/development/test/validation-and-review.md` for completion
and evidence reuse; activating this skill does not require another hygiene run.

Commit messages describe the technical result without AI attribution or validation
trailers. Verify the resulting commit and worktree. Use a normal push for additive
history, and verify that the remote ref matches the intended commit.

Create or update the existing PR with the correct base/head and a description of
the full branch diff. Follow any repository template. Monitor checks on the current
HEAD; superseded runs do not validate it. Diagnose failures before rerunning and
report unvalidated platforms. Merge only with separate authorization.
