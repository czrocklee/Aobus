---
name: write-tests
description: Write, restructure, or review Aobus C++/Objective-C++ and Python tests, or measure C++ coverage with ./ao measure coverage. Do not load merely to run existing test targets or diagnose a failing test.
---

# Write and review Aobus tests

Test review or coverage measurement is read-only unless test edits are requested.

- Route admission, owning layers, Catch2 naming/tags, fixtures,
  async/UIModel/GTK harnesses, and Bazel support tests through
  [testing policy](../../../doc/development/test.md)
  ([detailed references](../../../doc/development/test.md#detailed-references))
  and [deciding what to test and check](../../../doc/development/test/scope.md).
- Route `test/BUILD.bazel` ownership registration and file-split rules through
  [adding new files](../../../doc/development/test/validation-and-review.md#adding-new-files),
  cross-thread `[concurrency]` contracts through
  [concurrency and sanitizer validation](../../../doc/development/test/concurrency-and-sanitizer.md),
  and Linux Debug coverage through
  [coverage workflow](../../../doc/development/test/coverage-workflow.md).
- Silent `./ao measure coverage` traps
  ([generating and inspecting coverage](../../../doc/development/test/coverage-workflow.md#generating-and-inspecting-coverage)):
  ambient `LLVM_PROFILE_FILE` / `GCOV_PREFIX*` variables are stripped without
  warning, and `--scope <prefix>` narrows only the reported denominator, never
  instrumentation or execution.
