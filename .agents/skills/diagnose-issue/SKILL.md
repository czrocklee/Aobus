---
name: diagnose-issue
description: >-
  Diagnose and fix Aobus failures (compile errors, test failures, crashes, sanitizer reports,
  deadlocks, races, hangs, threading bugs). Stay focused on root cause — avoid cleanup,
  formatting, or refactors unless required to resolve the issue.
---

# diagnose-issue

Use this skill when the user reports a failure or asks to debug behavior. The goal is to find and fix the root cause with the shortest reliable feedback loop.

## Operating Rule

Stay focused on the failing behavior. Do not start documentation updates, formatting passes, lint cleanup, include cleanup, coverage work, dependency upgrades, broad refactors, or unrelated test rewrites while diagnosing. Do those only when they directly unblock the fix or the user explicitly asks.

If the fix touches C++ files, also load `generate-cpp-code` before editing. Use `use-clang-tidy` only when the user explicitly asks to diagnose linting, clang-tidy, lint cleanup, or clang-tidy findings in the current session; otherwise do not run lint validation.

## C2 Delegation Boundary

Diagnosis, root-cause selection, error-contract choices, and bug-fix design remain C3 work. Once the fix
(or a C3-decided regression test) is settled, *implementing* it can be delegated as a proposal via the
`execute-plan` skill (`.agents/skills/execute-plan/SKILL.md`); the worker may self-load `write-unit-test`
/ `improve-test-coverage` for conventions. C2 returns a validated patch that you (the chair) review,
apply, and re-validate on the real tree — it never lands a change itself, and never decides the diagnosis.

## Debugging Loop

1. Capture the exact failing command, test filter, input, log excerpt, signal, assertion, or stack trace.
2. Reproduce with the narrowest command possible from the repo root. Prefer preserving existing `/tmp/build/...` trees.
3. Read only the files on the failing path: the diagnostic location, the called API, the closest test, and nearby helpers.
4. Form one concrete hypothesis. Verify it with one focused command, trace, assertion, log, debugger session, or source read.
5. Make the smallest behavioral change that addresses the root cause.
6. Re-run the narrow failing command. Widen validation only after the narrow check passes.
7. Report the cause, the fix, and the validation result. Mention deferred nonfunctional work only if it remains necessary.

## Useful Commands

Prefer the project wrapper commands from the repository root:

```bash
./build.sh debug
./build.sh debug --clang
nix-shell --run "cmake --build /tmp/build/debug --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test \"test filter\""
```

Inspect build logs instead of rerunning full builds when a previous run already captured the failure:

```bash
tail -200 /tmp/build/debug/build.log
rg -n "error:|undefined reference|AddressSanitizer|ThreadSanitizer|SUMMARY|FAILED|SIG" /tmp/build/debug/build.log
```

Use `rg` for code search. Search narrowly by symbol, error text, test name, or assertion text.

## Compile And Link Errors

- Start from the first real compiler error in the log, not the cascade.
- Check the exact declaration, definition, namespace, include, target linkage, and CMake source list involved.
- If a generated or moved file is involved, verify `test/CMakeLists.txt` or the relevant target includes it.
- Do not run clang-format, include-cleaner, or tidy just because the compiler mentions formatting-adjacent code.
- After the fix, rebuild the smallest target that failed; run tests only if the compile fix can affect behavior.

## Failing Tests

- Re-run the single failing Catch2 test case or section when possible.
- Read the assertion and the production path it exercises before editing the test.
- Treat test failure as a product bug until proven otherwise. Do not weaken assertions to pass.
- Fix the test only when the expected behavior has intentionally changed or the test relies on an invalid assumption.
- Add a regression test only when the existing failing test does not already pin the bug.

## Crashes And Sanitizers

- Capture the crashing command, signal, sanitizer report, and top relevant stack frames.
- Prefer debug/sanitizer builds already produced by `./build.sh debug`.
- For AddressSanitizer/UBSan, trace ownership, lifetime, bounds, optional/result access, casts, and moved-from state.
- For crashes without a report, use `gdb` or `lldb` inside `nix-shell` only after a narrow reproducer exists.
- Avoid speculative rewrites. Prove the invalid object, pointer, index, enum, or lifetime before patching.

## Hangs And Threading Bugs

- First distinguish CPU spin, blocked wait, deadlock, starvation, missed callback, and test waiting on a condition that never becomes true.
- Capture where threads are blocked with a debugger backtrace or targeted logging before changing synchronization.
- Inspect lock ordering, callback reentrancy, executor affinity, subscription lifetime, condition-variable predicates, atomics, and shutdown paths.
- Prefer deterministic tests with immediate executors, fake callbacks, and explicit synchronization over sleeps.
- Do not add arbitrary sleeps as a fix. Timeouts are acceptable only as test guards or user-facing failure handling.

## Patch Discipline

- Keep the edit close to the broken code path.
- Avoid drive-by modernization, renames, formatting churn, documentation updates, and unrelated cleanup.
- If a wider refactor appears necessary, first prove why a local fix is incorrect or unsafe.
- Preserve user changes in the worktree and do not revert unrelated edits.
- Add or adjust focused tests when the defect was not already covered.

## Validation Scope

Use the narrowest passing check first, then widen according to risk:

- Compile error: rebuild the failing target.
- Unit failure: rerun the exact failing test, then the affected test binary if needed.
- Crash: rerun the crashing command under the same build mode.
- Threading fix: rerun the reproducer repeatedly or under the relevant sanitizer when available.
- Shared core behavior: run `./build.sh debug` after the focused check passes.

Run formatting, docs, or broad coverage only after the functional issue is fixed, and only when required by the user's requested deliverable. Run clang-tidy only when the user explicitly asks for linting, clang-tidy, lint cleanup, or clang-tidy findings in the current session.

## High-Stakes Diagnoses And Reviews (optional)

For most failures, diagnose solo with the loop above. When the call is genuinely high-stakes and a wrong judgment is costly — a subtle concurrency/lifetime root cause with competing hypotheses, an error-contract or architecture decision the fix forces, or reviewing a large/risky change — you may convene a multi-model **council** instead of deciding alone: load `run-council` and run it with `mode: review` (the change/diagnosis as the body), and — since this path is reserved for the genuinely high-stakes — `depth: full` so the panel also self-revises before you synthesize. It gathers a cross-vendor frontier panel that drafts, challenges each other, and revises, then you synthesize the final verdict from the dossier. It is **opt-in and expensive** — do not use it for routine bugs.
