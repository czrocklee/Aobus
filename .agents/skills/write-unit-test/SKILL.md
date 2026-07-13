---
name: write-unit-test
description: Write or review Aobus C++ tests using Catch2, FakeIt, deterministic runtime helpers, and the repository's layered test strategy. Use when adding tests, reviewing tests, improving test design, or deciding where behavior belongs across lib, runtime, uimodel, and linux-gtk.
---

# write-unit-test

Use this skill to write behavior-focused Aobus tests that are easy to read, deterministic to run, and placed at the lowest layer that proves the behavior.

Aobus tests are layered, not just coverage probes. Prefer pure `lib`, `runtime`, or `uimodel` tests for product behavior. Use `linux-gtk` tests only for GTK rendering, event binding, widget lifecycle, and targeted layout regressions.

## Core rule

Test the public behavior contract, not the current implementation. A good test proves something observable: return values, persisted state, emitted signals, callback payloads, rendered view state, widget state, errors, ordering, or lifecycle cleanup. Mutation tests must assert a postcondition.

When reviewing rather than writing, work from the smell list and final checklist in `doc/development/test/validation-and-review.md`.

## Documentation map

This skill is an agent-facing routing layer. The operational rules — naming, tags, assertion patterns, fixture design, Catch2 style — live in the human development docs and are deliberately not restated here. `doc/development/test.md` is the landing page; read only the reference that matches the task:

- `doc/development/test/layer-selection.md` — deciding whether a test belongs in `lib`, `runtime`, `uimodel`, `linux-gtk`, workflow, integration, or regression.
- `doc/development/test/naming-and-assertion.md` — test names, the layer→tag mapping, tag grammar, `SECTION` vs `TEST_CASE`, assertion quality, `REQUIRE` vs `CHECK`, and expected-value pinning.
- `doc/development/test/fixture-and-helper.md` — fixtures, helpers, FakeIt mocks, hand-written fakes, testability seams, test data, and filesystem setup.
- `doc/development/test/runtime-and-async.md` — deterministic executors, coroutine/lifetime tests, runtime service patterns, and subscription/callback checks.
- `doc/development/test/concurrency-and-sanitizer.md` — concurrency tags, race scenario matrices, and sanitizer validation.
- `doc/development/test/uimodel-and-gtk.md` — UI model policy tests, GTK adapter tests, widget harnesses, event emission, CSS, popovers, and geometry regression boundaries.
- `doc/development/test/validation-and-review.md` — adding test files to CMake, regression-test style, validation commands, review checklist, and common smells.
- `doc/development/test/coverage-workflow.md` — coverage reports and the measure-analyze-test-verify loop.
- `doc/development/test/test-suite.md` — `./ao test` suite organization, suite groups, and non-Catch2 suite behavior.

## Default workflow

1. Read the production API and the closest sibling test file.
2. Pick the lowest layer that can prove the behavior.
3. Name the test as a behavior contract (`"Component - behavior under condition"`) and tag it per `doc/development/test/naming-and-assertion.md`.
4. Arrange only necessary state.
5. Act once unless the contract is about repeated calls, ordering, or idempotence.
6. Assert observable outcomes and postconditions, with explicit expected values rather than recomputed ones.
7. Before adding shared helpers, search existing `*TestSupport.h` files and layer utilities such as `test/unit/RuntimeTestSupport.h`.
8. Add new test files to `test/CMakeLists.txt`.
9. Validate according to `doc/development/test/validation-and-review.md`.

## Layer quick map

- `lib`: algorithms, data layouts, storage, parsing, serialization, query evaluation, audio primitives, low-level utility behavior.
- `runtime`: service contracts, runtime state transitions, projections, workspace/view/playback behavior, subscriptions, async lifecycle.
- `uimodel`: UI policy, view-state projection, menu models, editor models, layout models, validation, field formatting, presentation decisions.
- `linux-gtk`: GTK construction, render binding, event-to-action routing, CSS, popovers/dialogs, lifecycle cleanup, targeted layout regressions.

When unsure between GTK and uimodel, prefer `uimodel` if the behavior can be expressed as `input state -> view state`.

## Validation

Use `doc/development/test/validation-and-review.md` as the source of truth for validation commands and scope.

Do not run clang-tidy for ordinary test changes unless the user explicitly asks for linting, clang-tidy, tidy cleanup, or lint findings. If requested, use `./ao tidy`.

If the task is explicitly about coverage percentage or missing lines, use the `improve-test-coverage` skill instead of guessing from source files.
