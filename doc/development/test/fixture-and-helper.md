---
id: development.test.fixture-and-helper
type: development
status: current
domain: development
summary: Defines test fixture, helper, fake, test-data, and filesystem practices.
---
# Fixtures, helpers, fakes, and test data

## Fixtures and helpers

Use fixtures to remove noise, not to hide behavior.

Create or extend a helper when a test repeatedly performs setup such as:

- LMDB temp environment and database creation.
- Track hot/cold payload construction.
- AppRuntime and ConfigStore creation.
- GTK application/window/present/drain/unset lifecycle.
- Deterministic executor and queued task control.

Prefer test bodies like:

```cpp
auto fixture = TrackStoreFixture{};
auto id = fixture.createTrack({.title = "Before", .year = 2024});

fixture.updateTrack(id, {.title = "After", .year = 2025});

auto const view = fixture.requireTrack(id);
CHECK(view.metadata().title() == "After");
CHECK(view.metadata().year() == 2025);
```

Over repeated low-level setup in every test.

Fixture defaults should be non-trivial enough to prove data flow. Avoid using only empty strings, `0`, or `false` unless the test is about empty/default behavior.

Shared support must keep its compile surface narrow.
Header definitions follow the pure-AST exceptions in the [C++ coding style](../coding-style.md); test support does not receive a broader exception.
Templates, dependent definitions, permitted compile-time definitions, empty bodies, and ordinary bodies containing one permitted direct AST statement may remain in a support header.
Concrete fixture construction, lifecycle, I/O, event driving, and stateful helper methods belong in a paired `.cpp` file compiled by the owning test-support target.
Split support by the behavior or layer that consumes it instead of creating an include-all compatibility header.

## Testability seams

Tests should observe public behavior, not bypass production privacy. Do not add
an ungoverned test-only private-access backdoor to a production type.

The following closed inventory contains the exceptional low-level seams. Each
has one narrow owner, deterministic semantics, an owning specification, and a
build guard that rejects production consumers outside its implementation
owner:

| Seam | Permitted evidence | Boundary |
|---|---|---|
| `library::detail::PhysicalStoreAccess` | Exact malformed library bytes and isolated Store representation state that no logical mutation can create. | Source-private header; production roots cannot include, define, or invoke it. |
| `WriteTransaction::Options::optInjectedCommitFailure` | Rollback and publication behavior for a native commit result that cannot be induced portably. | Data-only input; production roots cannot reference it, and it invokes no callback while locks are held. |
| `library::detail::OpenValidationMetrics` | Exact operation counts for the open gate's linear Track/manifest growth law. | Source-private observation; only `MusicLibrary` may record/reset it and production roots cannot consume it. |
| `lmdb::detail::ReadFaultInjection` | Admission, live-read, and writer-read ownership for a native LMDB fault without damaging a mapped environment. | Source-private, single-use, same-thread scope; destruction requires that the next read consumed it, and production roots cannot reference it. |

These seams do not authorize application behavior, arbitrary collaborator
control, or a general test API. Adding another seam requires updating this
closed inventory, its owning behavior document, and a positive/negative
mechanical boundary test in the same change.

When a behavior is hard to observe, use this order:

1. Move pure policy into a lower-layer model or helper that can be tested
   directly.
2. Add a normal public observation point when the state is part of the
   component or service contract.
3. Use constructor injection, callbacks, clocks, executors, or a small
   interface to drive the behavior through production code.
4. Add focused shared test support around public APIs when the setup is common.
5. Stop and review the design if none of these options fits.

Fixture helpers must not silently rewrite domain identity to make setup succeed.
For example, the strict library `addTrack()` helper preserves the supplied URI and exposes duplicate-URI conflicts, while the explicitly named `addTrackWithUniqueFixtureUri()` helper opts into deterministic private URI allocation and media-file mirroring for high-level playback fixtures.

## Catch2 style reminders

- Include `<catch2/catch_test_macros.hpp>` for basic assertions.
- Add matcher/generator headers only when used.
- Put tests in the namespace style used by neighboring files, commonly `namespace ao::<module>::test` or `namespace ao::gtk::test`.
- Separate top-level `TEST_CASE` blocks and `SECTION` blocks with blank lines.
- Keep helpers in an anonymous namespace unless shared across files.
- For `ao::Result<T>` failures, assert the result is false before reading `error()`.
- For exceptions, use `REQUIRE_THROWS_AS` when the exception type is contractually important.

## FakeIt and hand-written fakes

Use FakeIt or hand-written fakes at boundaries:

- Audio backends/providers.
- External service interfaces.
- Callback sources.
- Clocks and executors.
- Filesystem or platform seams.
- UI action resolvers.

Prefer real value objects, builders, parsers, stores, and pure model classes over mocks. Do not mock internal collaborators just to assert implementation calls.

Verify interactions only when the interaction itself is the contract, such as backend `stop()`, provider creation, cancellation signal delivery, or callback subscription cleanup.

When a mocked interface must be returned as `std::unique_ptr`, use existing test helpers where available instead of inventing ownership tricks.

## Filesystem and data

- Use `ao::test::TempDir` for unit tests needing files or LMDB storage.
- Use integration test data only for integration-style behavior such as real codecs or scanned audio files.
- Keep malformed fixtures tiny and locally explained by the test name/setup.
- Open test files in binary mode whenever assertions compare exact bytes or preserve existing bytes; Windows text mode translates line endings and invalidates byte-for-byte evidence.
- Restore permissions in RAII helpers when testing filesystem permission errors.
- Avoid relying on host services, real user config, network, or global machine state.

## Helper smell checklist

Consider adding or improving a helper when:

- The first third of the test is repeated setup.
- A future reader must understand LMDB/GTK/coroutine plumbing before understanding the behavior under test.
- Several tests construct the same runtime graph manually.
- Multiple files define slightly different versions of the same fake executor or widget fixture.

Do not add a fixture that hides the action or assertion. The behavior under test should still be visible in the test body.

Before adding another local helper, search for the narrow support header in the
owning test layer, such as `test/unit/TestFixtureSupport.h`, the focused runtime
support headers under `test/unit/runtime/`, or the focused GTK support headers
under `test/unit/linux-gtk/`.
Prefer converging repeated queued executors, runtime setup, LMDB setup, and GTK
window/present/drain/unset lifecycles into one shared helper when the behavior
is genuinely common, while keeping unrelated support in separate headers.
