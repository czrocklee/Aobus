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
- Do not run clang-tidy or load `use-clang-tidy` during the session unless the user explicitly asks for linting, clang-tidy, lint cleanup, or resolving clang-tidy findings in that session.
- If the user explicitly requests lint validation, use `./script/run-clang-tidy.sh` through `use-clang-tidy`. For a commit covering the current change set, run it with no arguments; for a partial commit or unrelated worktree changes, pass the intended files explicitly.
- Fix warnings by improving code first. Use narrow, check-specific `NOLINT` only for justified tool/API boundaries.

For any code change, run the narrowest meaningful build or test. Use `./build.sh debug` when there is no safer focused check.

If the user asks whether a change is ready, safe, or worth merging, load `code-review` and keep the review focused on correctness and regressions.

## 3. Mechanical Pre-Commit Delegation (optional, C0/C1)

The mechanical front-half of a commit — targeted formatting plus lint cleanup of the changed C++ — can be delegated to `script/agent/commit_flow.sh`. This skill stays the frontier (**C3**) layer: commit_flow makes the changed C++ review-ready and **never commits, stages, or runs any destructive git command**; the commit decision, message, and semantic review stay here.

What it runs over the changed C++ set: **C0** targeted `clang-format` → **C1** lint phase (a Phase Packet through `script/agent/dispatch.sh`, fixing to fixpoint under deterministic guards) → an independent `tidy` gate. Guarded paths (e.g. `include/**`, `*/CMakeLists.txt`) and anything the C1 worker cannot converge are escalated to you, never auto-edited.

**When to use it — opt-in, the same gate as §2:** only when lint/clang-tidy cleanup is explicitly part of this commit. It runs the clang-tidy phase, so for an ordinary commit do NOT run it — use the default path (the §1 format pass + §2 scoped build/test).

```bash
./script/agent/commit_flow.sh   # format + lint the changed C++ set; prints a hand-off, never commits
```

**Acting on its result:**
- **exit 0 — `READY FOR C3`:** the changed C++ is formatted and lint-clean (tidy gate passed). Its C0 step already covered the §4 format step — do not re-run the §1 loop. Its gate is tidy-only, so still run the §2 scoped build/test, then continue the Commit Procedure (review diff → message → stage → commit).
- **exit 2 — `NEEDS C3`:** resolve the listed escalation packets (under `$AOBUS_AGENT_WORK/lint/escalate/`, default `/tmp/aobus-agent/lint/escalate/`) and/or guarded paths first, then commit.

commit_flow mutates the working tree (formatting and lint fixes); review those edits in `git diff` before staging. The DANGER rules in §5 still apply.

## 3.1 C2 Proposal Review Handoff

C2 never keeps a change — it produces a **proposal** (a validated patch + dossier via the `execute-plan`
skill). Before staging or committing a C2-delegated change, read its review dossier, apply the patch to
the real tree, re-validate, perform C3 semantic review, and record the verdict:

```bash
script/agent/record_review.sh <phase-id> accept|reject [reason]
```

The phase id must already exist as a proposal audit entry, and verdicts are terminal: a conflicting
second verdict is rejected. Only an accepted change proceeds through the normal commit procedure.
Rejecting a `proposal-validated` phase is a **silent-wrong** that trips the per-worker circuit breaker
(cleared via `review_stats.sh --reset`); record honestly, and do not treat a passing oracle as sufficient
semantic approval.

## 4. Commit Procedure
1. Review the repo state with `git status`, `git diff HEAD`, and `git log -n 3`.
2. Confirm implementation and debugging are complete.
3. Format changed `.cpp`, `.h`, and `.hpp` files once with the targeted command above and report its output. (If you used `commit_flow.sh` for this commit, its C0 step already formatted them — do not re-run the loop.)
4. Run scoped validation for the relevant changed code and report pass/fail before staging or committing.
5. Stage only the intended changes.
6. Use an imperative commit message such as `perf: optimize TrackRow memory usage`. Focus on the primary technical contribution and substantive logic changes. Avoid generic labels like "style" or "chore" if the commit introduces new features, bug fixes, or significant refactorings; emphasize the core "what" and "why" over secondary stylistic cleanup. Do not reference project plans, design docs, or internal task IDs (e.g., avoid "implement phase 2 of plan X"). Do not append "Co-Authored-By" or any AI signatures.
7. Run `git status` after the commit and do not conclude until the working tree is clean or only unrelated user changes remain.

## 5. Scope And Safety

- **DANGER — `git checkout` / `git restore` / `git reset --hard` destroy uncommitted work without warning.** Before running these, confirm with the user and double-check there are no unstaged changes that matter.
- Do not widen the formatting or validation scope unless the user asked for cleanup beyond the current change.
- Do not silently fix unrelated violations found during formatting or validation.
- If the worktree contains unrelated user changes, operate around them and keep your reporting focused on the files relevant to the requested git task.
