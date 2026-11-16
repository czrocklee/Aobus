---
name: diagnose-issue
description: Reproduce and root-cause Aobus build failures, failing tests, crashes, hangs, and ASan/TSan failures; apply a fix when requested. Do not load for static code review or writing/reviewing test suites.
---

# Diagnose an Aobus issue

Diagnosis permits reproduction and inspection; source edits require a fix
request. Preserve the original reproducer—a passing retry never explains an
intermittent failure.

- Route target selection, `--test_filter` rules, and host prerequisites through
  [test suites](../../../doc/development/test/test-suite.md), and cross-thread
  races, deadlocks, or sanitizer failures through
  [concurrency and sanitizer validation](../../../doc/development/test/concurrency-and-sanitizer.md)
  ([sanitizer findings](../../../doc/development/test/concurrency-and-sanitizer.md#sanitizer-findings)).
- Executing an existing test binary directly is diagnostic reuse only: it
  neither validates edited C++ nor starts the private display, D-Bus, or audio
  services managed by `./ao check`.
- For `app/linux-gtk/` signal lifetimes, replaceable dependencies, or widget
  teardown, load `manage-gtk-lifetimes`.
