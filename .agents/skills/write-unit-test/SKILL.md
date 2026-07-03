---
name: write-unit-test
description: Write or review Aobus C++ tests using Catch2, FakeIt, deterministic runtime helpers, and the repository's layered test strategy. Use when adding tests, reviewing tests, improving test design, or deciding where behavior belongs across lib, runtime, uimodel, and linux-gtk.
---

# write-unit-test

Use this skill to write behavior-focused Aobus tests that are easy to read, deterministic to run, and placed at the lowest layer that proves the behavior.

Aobus tests are layered, not just coverage probes. Prefer pure `lib`, `runtime`, or `uimodel` tests for product behavior. Use `linux-gtk` tests only for GTK rendering, event binding, widget lifecycle, and targeted layout regressions.

## Core rule

Test the public behavior contract, not the current implementation. A good test proves something observable: return values, persisted state, emitted signals, callback payloads, rendered view state, widget state, errors, ordering, or lifecycle cleanup.

Mutation tests must assert a postcondition. Avoid tests that only prove a branch executed, a function returned truthy, or a callback was merely called.

When reviewing rather than writing, work from the smell list and final checklist in `doc/dev/testing/validation-and-review.md`.

## Documentation map

Testing knowledge lives in the human development docs. This skill is an agent-facing routing layer; read only the document that matches the task.

| Document | Purpose |
|---|---|
| `doc/dev/testing.md` | Testing policy landing page and detailed reference index |
| `doc/dev/testing/` | Detailed operational rules, code patterns, and checklists |

## On-demand references

Read only the development doc that matches the task. Do not load every reference by default.

- `doc/dev/testing/layer-selection.md` — deciding whether a test belongs in `lib`, `runtime`, `uimodel`, `linux-gtk`, workflow, integration, or regression.
- `doc/dev/testing/naming-and-assertions.md` — test names, tags, Arrange/Act/Assert style, and assertion quality.
- `doc/dev/testing/fixtures-and-helpers.md` — when to introduce fixtures, helpers, FakeIt mocks, hand-written fakes, test data, and filesystem setup.
- `doc/dev/testing/runtime-and-async.md` — deterministic executors, coroutine/lifetime tests, runtime service patterns, and subscription/callback checks.
- `doc/dev/testing/uimodel-and-gtk.md` — UI model policy tests, GTK adapter tests, widget harnesses, event emission, CSS, popovers, and geometry regression boundaries.
- `doc/dev/testing/validation-and-review.md` — adding test files to CMake, regression-test style, validation commands, review checklist, and common smells.
- `doc/dev/testing/coverage-workflow.md` — generating coverage reports, common Aobus coverage gaps, and the measure-analyze-test-verify loop.
- `doc/dev/testing/test-suites.md` — `./ao test` suite organization, suite groups, and non-Catch2 suite behavior.

## Default workflow

1. Read the production API and the closest sibling test file.
2. Pick the lowest layer that can prove the behavior.
3. Name the test as a behavior contract.
4. Arrange only necessary state.
5. Act once unless the contract is about repeated calls, ordering, or idempotence.
6. Assert observable outcomes and postconditions.
7. Before adding shared helpers, search existing `*TestSupport.h` files and layer utilities such as `test/unit/RuntimeTestUtils.h`.
8. Add new test files to `test/CMakeLists.txt`.
9. Run the narrowest useful test filter first when practical.

## Layer quick map

- `lib`: algorithms, data layouts, storage, parsing, serialization, query evaluation, audio primitives, low-level utility behavior.
- `runtime`: service contracts, runtime state transitions, projections, workspace/view/playback behavior, subscriptions, async lifecycle.
- `uimodel`: UI policy, view-state projection, menu models, editor models, layout models, validation, field formatting, presentation decisions.
- `linux-gtk`: GTK construction, render binding, event-to-action routing, CSS, popovers/dialogs, lifecycle cleanup, targeted layout regressions.

When unsure between GTK and uimodel, prefer `uimodel` if the behavior can be expressed as `input state -> view state`.

## Test naming and tags

Prefer:

```cpp
TEST_CASE("Component - behavior under condition", "[layer][type][component]")
```

Function-level tests may use:

```cpp
TEST_CASE("functionName returns result under condition", "[layer][unit][component]")
```

Recommended type tags include `[unit]`, `[workflow]`, `[integration]`, `[regression]`, and `[smoke]`.

Avoid vague names such as `"ActionRegistry"`, `"Library Export/Import Cycle"`, or `"Simple Equal Match"` for new tests.

## Assertion baseline

Weak:

```cpp
REQUIRE(result);
CHECK(optView.has_value());
CHECK(count == 3);
```

Better:

```cpp
REQUIRE(result);
CHECK(result->state == ExpectedState::Ready);

REQUIRE(optView.has_value());
CHECK(optView->metadata().title() == "After");
CHECK(optView->property().duration() == std::chrono::minutes{3});

REQUIRE(items.size() == 3);
CHECK(items[0].id == firstId);
CHECK(items[1].id == secondId);
CHECK(items[2].id == thirdId);
```

For callbacks, assert payloads, ordering, and non-emission when relevant. `called == true` is only enough when the event itself is the whole contract.

## Catch2 style

- Include `<catch2/catch_test_macros.hpp>` for basic assertions.
- Add matcher/generator headers only when used.
- Match the namespace style of neighboring tests, commonly `namespace ao::<module>::test` or `namespace ao::gtk::test`.
- Keep helpers in an anonymous namespace unless shared across files.
- Do not introduce duplicate shared helper types and avoid collisions by putting them in a nested namespace; reuse or extend existing support helpers instead.
- Choose `SECTION` vs `TEST_CASE` by failure isolation, not scenario count: separate `TEST_CASE`s for independent contracts, `SECTION` for variants sharing one arrange. Over-splitting duplicates the arrange and pushes plumbing into `*TestSupport.h` — see `doc/dev/testing/naming-and-assertions.md`.
- Use `REQUIRE` for preconditions that make later checks meaningless; use `CHECK` for independent observations after the action.
- Prefer explicit expected values over duplicating production algorithms.
- For `ao::Result<T>` failures, assert the result is false before reading `error()`.
- For exceptions, use `REQUIRE_THROWS_AS` when the exception type is contractually important.

## Validation

Recommended broad check from the project root:

```bash
./ao check
```

Focused iteration examples:

```bash
./ao test --core "Component - behavior"
./ao test --gtk "Component - behavior"
./ao test --integration "Component - behavior"
./ao test --council "Component - behavior"
```

Do not run clang-tidy for ordinary test changes unless the user explicitly asks for linting, clang-tidy, tidy cleanup, or lint findings. If requested, use:

```bash
./ao tidy
```

If the task is explicitly about coverage percentage or missing lines, use the `improve-test-coverage` skill instead of guessing from source files.
