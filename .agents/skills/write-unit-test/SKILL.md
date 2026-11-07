---
name: write-unit-test
description: Write, restructure, or review Aobus C++ and Python tests, including Bazel support tests, owning layers, fixtures, assertions, scope, and validation.
---

# Write Aobus tests

Review is read-only unless tests or fixes are requested.
Start with the [testing policy](../../../doc/development/test.md).
For Python tests of the Bazel support layer, follow
[Bazel support tests](../../../doc/development/test.md#bazel-support-tests)
before choosing fixtures, fakes, or assertions.

For C++ tests, prefer `lib`, `runtime`, or `uimodel` for behavior expressible
without GTK. GTK tests own rendering, event binding, widget lifecycle, and
layout boundaries.

Read only the applicable focused references:

- [Layer selection](../../../doc/development/test/layer-selection.md): owning layer, contract choices, and assertion direction.
- [Naming and assertions](../../../doc/development/test/naming-and-assertion.md): names, tags, assertions, and SECTION policy.
- [Fixtures and helpers](../../../doc/development/test/fixture-and-helper.md): where a new case belongs, helpers, fakes, and seams.
- [Runtime and async](../../../doc/development/test/runtime-and-async.md): deterministic executors and runtime subscriptions.
- [UIModel and GTK](../../../doc/development/test/uimodel-and-gtk.md): UI model policy and native GTK harnesses.
- [Concurrency and sanitizers](../../../doc/development/test/concurrency-and-sanitizer.md): synchronization and cancellation contracts.
- [Validation and review](../../../doc/development/test/validation-and-review.md): registering files, reviewing existing tests, and completion.
- [Performance](../../../doc/development/test/performance.md): `test/perf` workloads and reports.

For a task explicitly about measured coverage gaps, use `improve-test-coverage`.
