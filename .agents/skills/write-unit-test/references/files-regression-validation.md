# Files, regression tests, validation, and review reference

## Adding new files

When creating a new test file:

1. Place it under the matching existing `test/unit/...` module or layer directory, such as `runtime`, `uimodel`, `linux-gtk`, `library`, `query`, `tag`, `audio`, or `utility`.
2. Add it to the correct target in `test/CMakeLists.txt`.
3. Match namespace and include style of neighboring tests.
4. Include only headers that are used.
5. Keep file-scope helpers local unless multiple files need them.

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

Good split boundaries include:

- Query evaluator scalar, dictionary, tag, bloom-filter, range/list, and load-mode behavior.
- TrackStore raw layout and malformed buffer behavior vs create/read/update/delete behavior through fixtures.
- Activity status compact, detail, dismissal, and progress policy.
- Import/export round-trip correctness vs coordinator dialog/lifecycle glue.

Do not perform a broad test-file split as drive-by cleanup unless it is necessary for the current change or explicitly requested.

## Validation

Recommended broad check from the project root:

```bash
./ao check
```

Focused iteration after the test binary already exists:

```bash
./ao test --core "Component - behavior"
./ao test --gtk "Component - behavior"
./ao test --integration "Component - behavior"
./ao test --fleet "Component - behavior"
```

Run the narrowest useful filter first, then the normal project test target when practical.

Do not run clang-tidy for ordinary test changes unless the user explicitly asks for linting, clang-tidy, tidy cleanup, or lint findings in the current session. If requested, use:

```bash
./ao tidy
```

If the task is explicitly about coverage percentage or missing lines, use the `improve-test-coverage` skill instead of guessing from source files.

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
- Fixtures reduce noise without hiding the behavior under test.
- GTK tests do not duplicate policy better tested in `uimodel`.
- New files are listed in `test/CMakeLists.txt`.
- Focused validation has been run when practical, or skipped with an honest reason.
