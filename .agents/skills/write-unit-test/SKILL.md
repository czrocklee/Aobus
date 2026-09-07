---
name: write-unit-test
description: Write or review Aobus C++ tests and select their owning layer and fixtures.
---

# Write Aobus C++ tests

Review is read-only unless tests or fixes are requested. Prefer `lib`, `runtime`,
or `uimodel` for behavior expressible without GTK. GTK tests own rendering,
event binding, widget lifecycle, and layout boundaries.

Read only the applicable references under `doc/development/test/`:

- `layer-selection.md`: choosing the owning layer and suite.
- `naming-and-assertion.md`: Catch2 names, tags, assertions, and SECTION policy.
- `fixture-and-helper.md`: existing helpers, FakeIt, test data, and testability seams.
- `runtime-and-async.md`: deterministic executors and runtime subscriptions.
- `uimodel-and-gtk.md`: UI model policy and native GTK harnesses.
- `concurrency-and-sanitizer.md`: synchronization and cancellation contracts.

Add new C++ test files to `test/CMakeLists.txt`. Completion and coverage reuse
follow `doc/development/test/validation-and-review.md`.
For a task explicitly about measured coverage gaps, use `improve-test-coverage`.
