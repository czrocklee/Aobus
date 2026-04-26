---
name: manage-git-flow
description: "Manages RockStudio formatting and commit flow. Use before any git status, diff, staging, or commit work so the required targeted clang-format and history verification steps are followed."
---

# Manage Git Flow

Use this skill before performing any git operation in RockStudio.

## 1. Targeted Formatting
When a user asks to commit, you MUST run the following command strictly to apply `clang-format` ONLY on the modified or created files. Do NOT use global formatting scripts.

Run this command before performing any commit:
```bash
for f in $(git status --porcelain | awk '{print $2}'); do
  if [ -f "$f" ] && [[ "$f" =~ \.(cpp|h)$ ]]; then
    clang-format -i "$f"
    printf 'formatted %s\n' "$f"
  fi
done
```
Action required: report the command output in your response so the user can see exactly which files were formatted.

## 2. Changed-Line Conformance Check
After formatting and before staging or committing, review the edited lines against [CONTRIBUTING.md](file:///home/rocklee/dev/RockStudio/CONTRIBUTING.md). Keep the review scoped to the lines you changed unless the user asked for a broader cleanup.

Use this checklist:
- `2.1.2`: check spacing around control blocks and between related statement groups; add blank lines where the changed code reads more clearly with them.
- `3.2.6`: check whether newly written `if` or `switch` code should use an init-statement such as `if (auto value = get(); condition)`.
- `3.3.5`: check whether newly introduced non-primitive object declarations should prefer `auto x = T{...};` or `auto x = T{};`, while leaving simple null pointer declarations in the explicit pointer form described there.

## 3. Commit Procedure
1. Review the repo state with `git status`, `git diff HEAD`, and `git log -n 3`.
2. Format changed `.cpp` and `.h` files with the targeted `clang-format` command above and report its output.
3. Confirm the changed lines conform to [CONTRIBUTING.md](file:///home/rocklee/dev/RockStudio/CONTRIBUTING.md), especially `2.1.2`, `3.2.6`, and `3.3.5`.
4. Use an imperative commit message such as `perf: optimize TrackRow memory usage`.
5. Run `git status` after the commit and do not conclude until the working tree is clean.
