---
name: manage-git-flow
description: >-
  BLOCKING — Activate BEFORE git status/diff/log/add/commit/push/rebase/stash/checkout/reset/show/branch,
  or when the user says "commit"/"push"/"rebase"/"diff". Enforces targeted formatting,
  scoped validation, and no AI attribution in commits.
---

# Manage Git Flow

Use this skill before performing any git operation in Aobus.

## 1. Targeted Formatting
Do not run `clang-format` during normal implementation, debugging, validation, or final response prep. Formatting before the commit step makes diffs noisy and makes later patches harder to apply correctly.

Run formatting only when the user explicitly asks for formatting or when creating a commit. For commits, run one targeted `clang-format` pass on modified or created C++ files only, immediately before staging/committing. Do not use global formatting scripts.

Run this command before performing any commit:
```bash
while IFS= read -r -d '' entry; do
  st="${entry:0:2}"
  f="${entry:3}"

  if [[ "$st" =~ ^[RC] ]]; then
    IFS= read -r -d '' f || break
  fi

  if [ -f "$f" ] && { [[ "$f" == *.cpp ]] || [[ "$f" == *.h ]] || [[ "$f" == *.hpp ]]; }; then
    clang-format -i "$f"
    printf 'formatted %s\n' "$f"
  fi
done < <(git status --porcelain -z)
```

This form preserves spaces in filenames and handles rename/copy entries correctly.

Action required: report the command output in your response so the user can see exactly which files were formatted.

## 2. Scoped Validation
Before staging or committing, validate only the relevant changed code unless the user asked for broader cleanup. If the commit flow includes a formatting pass, validate after that pass.

For C++ changes:

- Load `generate-cpp-code` before editing `.cpp`, `.h`, or `.hpp` files.
- Load `use-clang-tidy` when linting C++ changes or resolving clang-tidy findings.
- Use `./script/run-clang-tidy.sh` through that skill. For a commit covering the current change set, run it with no arguments; for a partial commit or unrelated worktree changes, pass the intended files explicitly.
- Fix warnings by improving code first. Use narrow, check-specific `NOLINT` only for justified tool/API boundaries.

For any code change, run the narrowest meaningful build or test. Use `./build.sh debug` when there is no safer focused check.

If the user asks whether a change is ready, safe, or worth merging, load `code-review` and keep the review focused on correctness and regressions.

## 3. Commit Procedure
1. Review the repo state with `git status`, `git diff HEAD`, and `git log -n 3`.
2. Confirm implementation and debugging are complete.
3. Format changed `.cpp`, `.h`, and `.hpp` files once with the targeted command above and report its output.
4. Run scoped validation for the relevant changed code and report pass/fail before staging or committing.
5. Stage only the intended changes.
6. Use an imperative commit message such as `perf: optimize TrackRow memory usage`. Focus on the primary technical contribution and substantive logic changes. Avoid generic labels like "style" or "chore" if the commit introduces new features, bug fixes, or significant refactorings; emphasize the core "what" and "why" over secondary stylistic cleanup. Do not reference project plans, design docs, or internal task IDs (e.g., avoid "implement phase 2 of plan X"). Do not append "Co-Authored-By" or any AI signatures.
7. Run `git status` after the commit and do not conclude until the working tree is clean or only unrelated user changes remain.

## 4. Scope And Safety

- **DANGER — `git checkout` / `git restore` / `git reset --hard` destroy uncommitted work without warning.** Before running these, confirm with the user and double-check there are no unstaged changes that matter.
- Do not widen the formatting or validation scope unless the user asked for cleanup beyond the current change.
- Do not silently fix unrelated violations found during formatting or validation.
- If the worktree contains unrelated user changes, operate around them and keep your reporting focused on the files relevant to the requested git task.
