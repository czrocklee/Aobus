---
id: development.test.validation-and-review
type: development
status: current
domain: development
summary: Defines test-file integration, regression expectations, validation gates, and review checks.
---
# Test validation and review

## Adding new files

When creating a new test file:

1. Place it under the matching existing `test/unit/...` module or layer directory, such as `runtime`, `uimodel`, `linux-gtk`, `library`, `query`, `tag`, `audio`, or `utility`.
2. Add it to the correct target in `test/CMakeLists.txt`.
3. Match namespace and include style of neighboring tests.
4. Include only headers that are used.
5. Check the focused `*TestSupport.h` files in the owning layer before creating a new shared helper; do not add an include-all umbrella.
6. Keep file-scope helpers local unless multiple files need them.
7. Do not create duplicate helper types and hide the conflict in a nested namespace; reuse or extend the existing helper instead.

## Regression tests

Regression tests are encouraged when they protect a real bug, especially for:

- Query optimizer correctness.
- Async cancellation/lifetime cleanup.
- GTK widget destruction order.
- Layout measurement stability.
- Import/export data preservation.
- Parser/serializer edge cases.

Name and tag them as regressions when appropriate:

```cpp
TEST_CASE("ImportExportCoordinator - cancellation after worker completion does not post error",
          "[gtk][regression][import-export]")
```

Add a short comment if the assertion is non-obvious or protects a fragile UI/layout lifecycle invariant.

## Large test files

When adding to an already large test file, prefer a new focused file if the behavior belongs to a separable contract area. Split by behavior domain, not by arbitrary line count.

Prefer standalone `TEST_CASE`s for independent behavior contracts; keep `SECTION` for
variants of one contract that share an arrange (see *SECTION vs TEST_CASE* in
`naming-and-assertions.md`). Splitting duplicates the arrange, so route genuinely shared
setup into an existing `*TestSupport.h` rather than copying it — and do not hide a
duplicate helper behind a nested namespace to dodge a collision.
Keep its concrete implementation in the paired `.cpp` unless it must remain
visible for template or compile-time semantics.

Good split boundaries include:

- Query evaluator scalar, dictionary, tag, bloom-filter, range/list, and load-mode behavior.
- TrackStore raw layout and malformed buffer behavior vs create/read/update/delete behavior through fixtures.
- Activity status compact, detail, local hiding, and task-progress policy.
- Import/export round-trip correctness vs coordinator dialog/lifecycle glue.

Do not perform a broad test-file split as drive-by cleanup unless it is necessary for the current change or explicitly requested.

## Validation

Completed work runs both parts of the native completion gate from the project
root, in this order:

```bash
./ao check
./ao hygiene
```

`check` builds the enabled graph, verifies dependency resolution, and runs
every suite in the native `all` group. `hygiene` is check-only and validates
formatting, repository source audits, Python files in scope, and native
clang-tidy coverage for changed files. Keeping the stages explicit lets
sanitizer and release checks retain their own build trees without implicitly
provisioning a second tidy tree.

Concurrency-sensitive changes additionally follow `concurrency-and-sanitizer.md` and run:

```bash
./ao test --concurrency
./ao check --tsan
```

Focused filters are debugging tools, not routine validation. Use them only when
a concrete failure or hypothesis needs a tighter feedback loop:

```bash
./ao test --core "Component - behavior"
./ao test --gtk "Component - behavior"
./ao test --integration "Component - behavior"
```

Do not run a ladder of suite filters as a substitute for the completion gate.

Do not run clang-tidy for ordinary test changes unless the user explicitly asks for linting, clang-tidy, tidy cleanup, or lint findings in the current session. If requested, use:

```bash
./ao tidy
```

If the task is explicitly about coverage percentage or missing lines, use `coverage-workflow.md` instead of guessing from source files.

## Common smells to fix while writing tests

- The test name only says the class name.
- A mutation test has no read-back assertion.
- The test checks only `has_value()` or `count` when content matters.
- A GTK test duplicates detailed policy already covered in `uimodel`.
- A pure rule is tested through a full widget tree.
- A runtime unit test performs a full filesystem workflow without being marked workflow/integration.
- The test uses sleep/yield to make async behavior pass.
- The same fixture setup is copied into many cases.
- The expected value is computed by duplicating the production algorithm.
- The test relies on incidental widget hierarchy or display text when a stable accessor would be better.
- The test violates the testability seam order in
  [fixtures and helpers](fixture-and-helper.md).
- Commented-out assertions or stale TODO comments remain in the test.
- A new case is appended to a giant test file even though it has a clear separate contract area.

## Final review checklist

Before finishing, confirm:

- The test lives at the lowest layer that proves the behavior.
- The name explains behavior and condition.
- Tags identify layer, type, and component.
- The test asserts observable outcomes, not implementation details.
- Mutations have postconditions.
- Async/GTK behavior is deterministic.
- Concurrent cancellation and teardown cover the applicable race matrix.
- Threading changes pass the baselined TSan gate.
- Fixtures reduce noise without hiding the behavior under test.
- GTK tests do not duplicate policy better tested in `uimodel`.
- Testability seams follow [fixtures and helpers](fixture-and-helper.md).
- New files are listed in `test/CMakeLists.txt`.
- Focused validation has been run when practical, or skipped with an honest reason.
- The native `./ao check` and subsequent `./ao hygiene` both pass.
