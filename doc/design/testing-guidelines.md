# Testing Guidelines

Aobus tests are behavior contracts, not coverage probes. Prefer small tests that
prove observable behavior at the lowest layer that can express the contract.

This document defines the default style for new tests and for tests touched by
maintenance work. Existing legacy tests do not need mechanical rewrites.

## Layer Selection

Choose the lowest layer that proves the behavior:

- `lib`: pure algorithms, storage, parsing, serialization, query evaluation, audio primitives, and utility behavior.
- `runtime`: service contracts, runtime state transitions, projections, subscriptions, callbacks, and async lifecycle.
- `uimodel`: UI policy, view-state projection, menu models, editor models, validation, field formatting, and presentation decisions.
- `linux-gtk`: GTK construction, render binding, event-to-action routing, CSS, popovers/dialogs, lifecycle cleanup, and targeted layout regressions.

When choosing between `uimodel` and GTK, prefer `uimodel` if the behavior can be
expressed as input state to view state. GTK tests should stay thin and prove only
adapter behavior.

## Naming And Tags

Prefer behavior names with layer, type, and component tags:

```cpp
TEST_CASE("TrackStore - updating metadata persists replacement values",
          "[library][unit][track-store]")
```

Recommended type tags:

- `[unit]`: a narrow behavior contract in one layer.
- `[workflow]`: multiple production components exercised inside a unit-suite binary.
- `[regression]`: a known bug or fragile invariant.
- `[smoke]`: a thin end-to-end sanity check.
- `[integration]`: true integration-target behavior or real external/backend dependencies.

Use `[integration]` deliberately. Heavy tests that still run in core or GTK unit
targets usually fit `[workflow]`, `[regression]`, or `[smoke]` better.

## Assertion Quality

Every test should assert an observable outcome: return values, persisted state,
emitted signals, callback payloads, rendered view state, widget state, error
content, ordering, or lifecycle cleanup.

Mutation tests must assert postconditions through the public API after the
mutation commits or completes. Avoid tests that only assert a branch executed, a
call returned truthy, or a callback was called.

Weak:

```cpp
REQUIRE(result);
CHECK(track.has_value());
CHECK(count == 3);
```

Better:

```cpp
REQUIRE(result);

auto track = store.read(id);
REQUIRE(track.has_value());
CHECK(track->metadata().title() == "After");
CHECK(track->properties().duration() == std::chrono::minutes{3});

REQUIRE(items.size() == 3);
CHECK(items[0].id == firstId);
CHECK(items[1].id == secondId);
CHECK(items[2].id == thirdId);
```

Use `REQUIRE` for preconditions that make later checks meaningless. Use `CHECK`
for independent observations after the action.

## Fixtures And Helpers

Helpers should remove repetitive setup, not hide the behavior under test.

- Keep helpers local in an anonymous namespace unless multiple files need them.
- Introduce a shared helper only after the same plumbing appears in multiple files.
- Do not create a test DSL that hides the act/assert steps.
- Prefer explicit expected values over duplicating production algorithms.

For storage tests, separate normal behavior helpers from raw layout or malformed
buffer tests. Raw bytes are appropriate when the layout is the contract; they are
noise in create/read/update/delete behavior tests.

## Runtime And Async Tests

Runtime tests should use deterministic executors, barriers, explicit callback
progression, and fake services instead of local sleep/yield loops.

Timeout-based waiting is acceptable only when the timing is centralized in a test
helper with useful diagnostics. Avoid scattering ad hoc waits through individual
test cases.

Callback tests should assert payloads, ordering, and non-emission when those are
part of the contract. `called == true` is only enough when the event itself is the
whole behavior.

## UI Model And GTK Tests

UI policy belongs in `uimodel` whenever possible. Feed model-like state in and
assert view state out. Cover priority, fallback, grouping, dismissal, validation,
idempotence, and signal behavior there.

GTK tests should verify adapter behavior:

- Widget construction and model/runtime binding.
- Runtime/model state rendered into labels, visibility, CSS classes, sensitivity, or popover state.
- User events routed to the expected action or state mutation.
- Lifecycle cleanup for known regressions.
- A small number of named geometry invariants.

GTK harnesses may own plumbing such as application setup, windows, mounting,
presenting, event draining, unmounting, and runtime fixture setup. They must not
hide user actions, business assertions, or widget lookup semantics.

Geometry tests should explain the invariant they protect and avoid theme- or
pixel-dependent assertions unless the exact size request is the component
contract. See `doc/design/gtk-testing-guidelines.md` for GTK-specific detail.

## Large Test Files

Split large files by behavior domain, not by arbitrary line count.

Good split boundaries include:

- TrackStore create/read/update/delete behavior vs raw layout and malformed buffer behavior.
- PlanEvaluator scalar, range/list, dictionary, tag, bloom-filter, and load-mode behavior.
- ActivityStatusModel compact, notification, detail, progress, and dismissal policy.
- Import/export round-trip correctness vs GTK coordinator dialog/lifecycle glue.

Do not perform broad test-file splits as drive-by cleanup. Strengthen shallow
assertions first, then split once the behavior boundaries are clear.

## Validation

Run the narrowest useful filter first, then broader checks when practical:

```bash
./ao test --core "Component - behavior"
./ao test --gtk "Component - behavior"
./ao test --integration "Component - behavior"
```

Use `./ao check` for the broad gate. Do not run format or tidy mid-session unless
explicitly requested or preparing a commit gate.
