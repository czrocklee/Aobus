---
name: manage-git-flow
description: Manage Aobus Git branches, commits, history rewrites, pushes, and pull requests. Load before creating a branch or mutating Git history or PRs; do not load for read-only git status, diff, or log inspection.
---

# Manage Aobus Git flow

Preserve unrelated worktree changes and work on a topic branch unless requested
otherwise. Repository hooks remain authoritative unless the user explicitly
authorizes a bypass for the current operation; keep an authorized bypass local to
that single operation and never disable hooks persistently or infer permission
from a general commit or publish request. If a hook blocks or resets an
operation, stop and report. Destructive `checkout`, `restore`, or `reset`
commands require explicit approval, and the default branch must never be
rewritten.

## History and rewrite authorization

Preserve the user's chosen history shape, separate independently useful changes,
and do not squash merely to reduce commit count.

On an agent-managed topic branch or PR, authorization to commit and publish
includes standing authorization to amend your own published commits for
same-intent follow-ups (including review fixes) without per-iteration approval;
when unpublished or covered by that rule, amend the owning commit, and otherwise
push an additive commit normally. Standing authorization never covers folding
unrelated changes, rewriting another contributor's commits, or discarding
unexpected remote work—those require explicit approval (an explicit amend or
squash request supplies it).

Before rewriting published history, record the intended range and observed
remote OID, then push with an OID-bound lease (if it fails, inspect the remote
change rather than weakening the lease):

```bash
git push --force-with-lease=<remote-ref>:<observed-remote-oid> <remote> <local-ref>:<remote-ref>
```

## Commits and pull requests

Route commit formatting through [commit-message guidelines](../../../doc/development/commit-message.md)
and PR preparation (including manual [`.github/pull_request_template.md`](../../../.github/pull_request_template.md)
adherence when passing `gh pr create --body` or `--body-file`) through [prepare a review](../../../CONTRIBUTING.md#prepare-a-review).

Monitor CI checks on the current `HEAD`—superseded runs do not validate it.
Commit, push, or PR authorization never authorizes merging or changing
repository rules.
