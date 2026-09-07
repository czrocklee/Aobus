---
name: diagnose-issue
description: Diagnose Aobus build failures, failing tests, crashes, and hangs; apply a root-cause fix when requested.
---

# Diagnose an Aobus issue

A diagnosis request permits reproduction and inspection; code changes require a
fix request. Keep work on the failing path and preserve the original reproducer
and build logs. A passing retry alone does not explain an intermittent failure.

Use the native portal's focused suite/filter or existing build log before
requesting another full build. `./ao test --help` lists the host's suites;
`-n` reuses existing binaries and does not validate changed C++.

For a race, deadlock, or asynchronous teardown failure, use `review-concurrency`.
For GTK ownership or signal rebinding, use `managing-gtk-lifetimes`.

Completion scope and reuse of evidence are owned by
`doc/development/test/validation-and-review.md`. Report the demonstrated cause,
any fix, and the remaining uncertainty; diagnosis alone does not require a
fresh product build.
