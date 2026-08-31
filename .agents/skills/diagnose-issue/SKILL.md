---
name: diagnose-issue
description: >-
  Diagnose Aobus compile errors, test failures, crashes, sanitizer reports, deadlocks, races, and
  hangs and, when explicitly requested, apply the smallest root-cause fix. Avoid unrelated cleanup,
  formatting, and refactors.
---

# Diagnose an issue

## Boundary

Diagnosis is read-only by default. Reproducing failures and inspecting generated diagnostics are
allowed, but do not edit tracked code or tests unless the user also asks to fix or resolve the
problem.

Stay on the failing path. Do not start documentation, formatting, lint, include, coverage,
dependency, or broad refactor work unless it directly unblocks the requested fix. Follow
`doc/development/test/validation-and-review.md`; activate `use-clang-tidy` only for an explicit lint
request.

Examples below use the Linux portal. On macOS follow `doc/development/macos.md`; on Windows use the
equivalent `ao.bat` command and the `develop-aobus-on-windows` skill.

## Loop

1. Capture the exact failing command, input, diagnostic, assertion, signal, or stack trace.
2. Reproduce that failure from the repository root, preserving existing build trees and logs.
3. Read the diagnostic location, called API, closest test, and nearby helpers; avoid unrelated code.
4. Form one concrete hypothesis and prove or reject it with one focused source read, trace, debugger
   session, assertion, or command.
5. For diagnosis-only work, stop at the proven cause and report the evidence.
6. When a fix was requested, make the smallest behavioral correction, add a regression only when
   existing coverage does not pin the defect, and re-run the original reproducer.
7. Complete the repository validation gate for code changes and report the cause, fix, and results.

## Failure-specific checks

- **Compile/link:** start at the first real error; verify declaration, definition, namespace,
  include, target linkage, and source lists before editing.
- **Test:** read the assertion and production path first. Treat it as a product defect until proven
  otherwise; never weaken an assertion merely to pass.
- **Crash/sanitizer:** capture the report and relevant frames, then prove the invalid ownership,
  lifetime, bounds, cast, or state transition before patching.
- **Hang/concurrency:** use `review-concurrency`; distinguish spin, blocked wait, deadlock,
  starvation, missed callback, and unmet condition before changing synchronization. Never use sleep
  as the fix.

## Useful Linux commands

```bash
./ao check
./ao check --clang
./ao test --core "test filter"
./ao test --gtk "test filter"
./ao test --integration "test filter"
```

Prefer an existing build log over rerunning a full build:

```bash
debug_build_dir="${BUILD_DIR:-${AOBUS_BUILD_ROOT:-/tmp/build}/$(basename "$PWD")/debug}"
tail -200 "$debug_build_dir/build.log"
rg -n "error:|undefined reference|AddressSanitizer|ThreadSanitizer|SUMMARY|FAILED|SIG" \
  "$debug_build_dir/build.log"
```

For a code fix, use focused commands only while iterating, then follow the completion policy in
`doc/development/test/validation-and-review.md`. A diagnosis-only task reports its reproducer and
evidence without manufacturing a completion build for unchanged code.
