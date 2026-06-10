---
name: write-unit-test
description: Write or review Aobus C++ unit tests using Catch2, FakeIt, and the repository's test conventions. Use when adding tests, fixing failing tests, improving test quality, or deciding what behavior and edge cases to cover outside the coverage-specific workflow.
---

# write-unit-test

Use this skill to write behavior-focused Aobus unit tests that match the existing Catch2 + FakeIt style.

If the task also modifies C++ source or test files, follow `generate-cpp-code` first, especially `references/04-test-snippets.md`.

## Delegating implementation

Deciding what behavior and edge cases to test remains chair judgment. Writing an already-decided test
plan may be delegated through the **`execute-plan`** skill with `task-kind: implement-plan`, exact test
paths in `scope`, and a correctness invariant. The fleet returns a proposal for chair review and never
lands it automatically.

## Core Workflow

1. Read the target production API and the closest sibling test file before writing tests.
2. Identify the public behavior contract: inputs, observable outputs, state changes, emitted callbacks, errors, and boundary behavior.
3. Add the smallest useful test surface in `test/unit/<module>/...Test.cpp`; use `test/integration/` only when real files, codecs, daemons, or cross-component wiring are the behavior under test.
4. Add the new test file to `test/CMakeLists.txt` when creating one.
5. Validate with the narrowest useful test filter first, then run the normal project test target when practical.

## Test Design

- Test behavior through public APIs, not private helpers or current implementation structure.
- Prefer one clear scenario per `SECTION`; keep shared setup at the `TEST_CASE` level only when every section needs it.
- Use non-trivial values so the test proves data flow, not default initialization. Avoid asserting only `0`, empty strings, or `false` unless testing an empty/boundary case.
- Cover success, invalid input, empty input, ordering, duplicates, out-of-range IDs, malformed buffers, callback paths, and error propagation as applicable.
- Keep tests deterministic: inject executors, callbacks, fake sessions, temp directories, or mocks instead of relying on wall-clock time, threads, global state, network, or host services.
- Avoid duplicating production algorithms in expected-value calculations. Prefer small known examples with explicit expected results.
- Use `REQUIRE` for preconditions that make later checks meaningless; use `CHECK` for independent observations after the action.

## Catch2 Style

- Include `<catch2/catch_test_macros.hpp>` for basic assertions.
- Add matcher/generator headers only when used.
- Name tests like `TEST_CASE("Component - behavior", "[domain][unit][component]")`.
- Put tests in the same namespace style as neighboring tests, commonly `namespace ao::<module>::test`.
- Separate top-level `TEST_CASE` blocks and nested `SECTION` blocks with blank lines.
- Keep fixtures local to the test file unless multiple files need the helper.
- For `ao::Result<T>` failures, assert the result is false before reading `error()`.
- For exceptions, prefer `REQUIRE_THROWS_AS` when the exception type is part of the contract.

## FakeIt Guidance

Use FakeIt for boundaries: audio backends, providers, callbacks, external services, clocks, filesystem seams, or interfaces that would make a unit test slow or nondeterministic.

Prefer real value objects, builders, stores, parsers, and pure logic over mocks. Do not mock an internal collaborator just to mirror implementation calls.

Common Aobus patterns:

- Include `<fakeit.hpp>` and use `fakeit::Mock<Interface>`.
- Use `fakeit::When(Method(mock, method)).AlwaysReturn(...)` for stable query behavior.
- Use `AlwaysDo(...)` to capture callbacks or return move-only/proxy values.
- Use `fakeit::Fake(Method(mock, method))` for benign void calls when the call itself is not the assertion.
- Verify interactions only when the interaction is the behavior contract, e.g. backend `stop()`/`close()` or provider `createBackend()`.
- When a mocked interface must be returned as `std::unique_ptr`, use an existing proxy helper such as `test/unit/audio/TestUtility.h` instead of inventing ownership tricks.

## Async And Callback Tests

- Prefer a local executor that runs `dispatch`/`defer` immediately when testing runtime services.
- Capture subscription callbacks in variables, trigger them explicitly, and then assert observable state or emitted events.
- Store returned subscription objects in named variables when their lifetime keeps callbacks connected.
- Make callback assertions specific enough to reject wrong event payloads, not just "called".

## Filesystem And Data

- Use test data under `test/integration/.../test_data` only for integration-style behavior.
- For unit tests, construct minimal in-memory buffers or use temp directories.
- Clean up temp directories, or use an existing RAII helper if the module already has one.
- Keep malformed binary fixtures tiny and locally explained by the test name or setup.

## Validation

Recommended checks from the project root:

```bash
./build.sh debug
```

For a focused iteration after the test binary already exists:

```bash
nix-shell --run "/tmp/build/debug/test/ao_test \"Component - behavior\""
```

Do not run clang-tidy for test changes unless the user explicitly asks for linting, clang-tidy, or lint cleanup in the current session. If explicitly requested, use:

```bash
./script/run-clang-tidy.sh
```

If the goal is coverage improvement rather than ordinary unit-test quality, use `improve-test-coverage` instead of guessing missing lines.
