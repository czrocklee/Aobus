# Testing

Aobus tests are behavior contracts, not coverage probes. Prefer small tests that
prove observable behavior at the lowest layer that can express the contract.

This document defines the default style for new tests and for tests touched by
maintenance work. Existing legacy tests do not need mechanical rewrites.

For detailed operational rules (naming examples, assertion patterns, code
samples), see the focused documents under `doc/dev/testing/`.

## Detailed References

| Document | Scope |
|---|---|
| `doc/dev/testing/layer-selection.md` | Layer boundaries and workflow/integration/regression placement |
| `doc/dev/testing/naming-and-assertions.md` | Test names, tags, structure, and assertion quality |
| `doc/dev/testing/fixtures-and-helpers.md` | Fixtures, helpers, FakeIt, test data, and filesystem setup |
| `doc/dev/testing/runtime-and-async.md` | Deterministic async, runtime service, callback, and lifetime tests |
| `doc/dev/testing/uimodel-and-gtk.md` | UI model policy tests, GTK adapter tests, geometry, lifecycle, and testability |
| `doc/dev/testing/coverage-workflow.md` | Coverage reports and the measure-analyze-test-verify loop |
| `doc/dev/testing/validation-and-review.md` | New files, regression tests, validation, smells, and review checklist |
| `doc/dev/testing/test-suites.md` | `./ao test` suite organization |

## Layer Selection

Choose the lowest layer that proves the behavior:

- `lib`: pure algorithms, storage, parsing, serialization, query evaluation, audio primitives, and utility behavior.
- `runtime`: service contracts, runtime state transitions, projections, subscriptions, callbacks, and async lifecycle.
- `uimodel`: UI policy, view-state projection, menu models, editor models, validation, field formatting, and presentation decisions.
- `linux-gtk`: GTK construction, render binding, event-to-action routing, CSS, popovers/dialogs, lifecycle cleanup, and targeted layout regressions.

When choosing between `uimodel` and GTK, prefer `uimodel` if the behavior can be
expressed as input state to view state. GTK tests should stay thin and prove only
adapter behavior.

For detailed layer selection guidance with boundary-case tables, see
`doc/dev/testing/layer-selection.md`.

## Naming and Tags

Name tests as behavior contracts: `"Component - behavior under condition"`.
Tag with `[layer][type][subsystem]`, keeping the total at 3–4 tags.

For tag tables, naming examples, and anti-patterns, see
`doc/dev/testing/naming-and-assertions.md`.

## Assertion Quality

Every test should assert observable outcomes: return values, persisted state,
emitted signals, callback payloads, or rendered view state. Mutation tests must
assert postconditions through the public API.

For assertion patterns and `REQUIRE` vs `CHECK` guidance, see
`doc/dev/testing/naming-and-assertions.md`.

## Fixtures and Helpers

Helpers should remove repetitive setup, not hide the behavior under test.
Search existing `*TestSupport.h` files before creating new shared helpers.
Prefer explicit expected values over duplicating production algorithms.
For helper file naming, use `doc/dev/naming-conventions.md`.

For fixture patterns and FakeIt guidance, see
`doc/dev/testing/fixtures-and-helpers.md`.

## Runtime and Async Tests

Use deterministic executors, barriers, and explicit callback progression instead
of sleep/yield loops. Assert callback payloads and non-emission, not just
`called == true`.

For async test patterns and executor examples, see
`doc/dev/testing/runtime-and-async.md`.

## UI Model and GTK Tests

UI policy belongs in `uimodel` whenever possible. GTK tests verify adapter
behavior: widget binding, event routing, lifecycle cleanup, and targeted geometry
invariants.

For GTK-specific rules including the "Three Questions" framework, geometry
tests, lifecycle rules, and testability advice, see
`doc/dev/testing/uimodel-and-gtk.md`.

## Large Test Files

Split by behavior domain, not by line count. Prefer separate `TEST_CASE`s for
independent contracts; use `SECTION` for variants sharing one arrange.

For split guidelines and the review checklist, see
`doc/dev/testing/validation-and-review.md`.

## Validation

Run the narrowest useful filter first, then broader checks when practical:

```bash
./ao test --core "Component - behavior"
./ao test --gtk "Component - behavior"
./ao check
```

Do not run format or tidy mid-session unless explicitly requested or preparing a
commit gate.

For coverage-specific workflow, see `doc/dev/testing/coverage-workflow.md`.
