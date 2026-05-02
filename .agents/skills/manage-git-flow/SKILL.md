---
name: manage-git-flow
description: "Manages Aobus formatting, conformance validation, and commit flow. Use before any git status, diff, staging, or commit work so the required targeted clang-format, delegated standards checks, and history verification steps are followed."
---

# Manage Git Flow

Use this skill before performing any git operation in Aobus.

## 1. Targeted Formatting
When a user asks to commit, you MUST run a targeted `clang-format` pass on modified or created C++ files only. Do NOT use global formatting scripts.

Run this command before performing any commit:
```bash
while IFS= read -r -d '' entry; do
  status="${entry:0:2}"
  f="${entry:3}"

  if [[ "$status" =~ ^[RC] ]]; then
    IFS= read -r -d '' f || break
  fi

  if [ -f "$f" ] && [[ "$f" =~ \.(cpp|h)$ ]]; then
    clang-format -i "$f"
    printf 'formatted %s\n' "$f"
  fi
done < <(git status --porcelain -z)
```

This form preserves spaces in filenames and handles rename/copy entries correctly.

Action required: report the command output in your response so the user can see exactly which files were formatted.

## 2. Conformance Validation
After formatting and before staging or committing, use the `check-code-conformance` skill instead of maintaining a separate inline checklist here.

Required behavior:

1. Load `check-code-conformance`.
2. Keep the conformance pass scoped to the files or changed lines you touched unless the user asked for a broader cleanup.
3. For C++ edits that are being validated for merge or commit readiness, let that skill decide whether to run `clang-tidy` through `./build.sh ... --tidy` and reuse the existing `/tmp/build/...-clang-tidy` tree when possible.
4. Report the conformance outcome clearly before staging or committing.

Do not duplicate coding-standard rules in this skill. `check-code-conformance` is the single source of truth for standards checks.

## 3. Commit Procedure
1. Review the repo state with `git status`, `git diff HEAD`, and `git log -n 3`.
2. Format changed `.cpp` and `.h` files with the targeted `clang-format` command above and report its output.
3. Run the `check-code-conformance` skill for the relevant changed code and report the result.
4. Use an imperative commit message such as `perf: optimize TrackRow memory usage`.
5. Run `git status` after the commit and do not conclude until the working tree is clean.

## 4. Scope And Safety

- Do not widen the formatting or conformance scope unless the user asked for cleanup beyond the current change.
- Do not silently fix unrelated violations found during formatting or conformance checks.
- If the worktree contains unrelated user changes, operate around them and keep your reporting focused on the files relevant to the requested git task.
