---
name: write-unit-test
description: Write, restructure, or review Aobus C++ tests, including their owning layer, fixtures, scope, and tags.
---

# Write Aobus C++ tests

Review is read-only unless tests or fixes are requested. Prefer `lib`, `runtime`,
or `uimodel` for behavior expressible without GTK. GTK tests own rendering,
event binding, widget lifecycle, and layout boundaries.

Read only the applicable references under `doc/development/test/`:

- `layer-selection.md`: owning layer and unit/integration scope.
- `naming-and-assertion.md`: names, tags, assertions, and SECTION policy.
- `fixture-and-helper.md`: where a new case belongs, helpers, fakes, and seams.
- `runtime-and-async.md`: deterministic executors and runtime subscriptions.
- `uimodel-and-gtk.md`: UI model policy and native GTK harnesses.
- `concurrency-and-sanitizer.md`: synchronization and cancellation contracts.
- `validation-and-review.md`: registering files, reviewing existing tests, and completion.
- `performance.md`: `test/perf` workloads and reports.

For a task explicitly about measured coverage gaps, use `improve-test-coverage`.
