---
name: diagnose-issue
description: Diagnose Aobus build failures, failing tests, crashes, and hangs; apply a root-cause fix when requested.
---

# Diagnose an Aobus issue

A diagnosis request permits reproduction and inspection; code changes require a
fix request. Keep work on the failing path and preserve the original reproducer
and build logs. A passing retry alone does not explain an intermittent failure.

Use a focused native Bazel target/filter or existing log before requesting
another full build. `doc/development/test/test-suite.md` owns selection and
native prerequisites. Filter the actual owning binary with `--test_filter`;
a zero-match binary fails. Running an existing artifact directly is diagnostic
reuse, not evidence for changed C++ or automatic service preparation.

For a race, deadlock, or asynchronous teardown failure, use `review-concurrency`.
For GTK ownership or signal rebinding, use `managing-gtk-lifetimes`.

Completion scope and reuse of evidence are owned by
`doc/development/test/validation-and-review.md`. Report the demonstrated cause,
any fix, and the remaining uncertainty; diagnosis alone does not require a
fresh product build.
