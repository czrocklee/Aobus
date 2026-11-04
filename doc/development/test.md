---
id: development.test
---
# Testing policy

Aobus tests are behavior contracts, not coverage probes. Prefer small tests that
prove observable behavior at the lowest layer that can express the contract.

This document defines the default style for new tests and for tests touched by
maintenance work. Existing legacy tests do not need mechanical rewrites.

For detailed operational rules (naming examples, assertion patterns, code
samples), see the focused documents under `doc/development/test/`.

## Detailed references

| Document | Scope |
|---|---|
| [`test/layer-selection.md`](test/layer-selection.md) | Layer boundaries and contract/assertion-layer choices |
| [`test/naming-and-assertion.md`](test/naming-and-assertion.md) | Test names, tags, structure, and assertion quality |
| [`test/fixture-and-helper.md`](test/fixture-and-helper.md) | Fixtures, helpers, FakeIt, test data, and filesystem setup |
| [`test/runtime-and-async.md`](test/runtime-and-async.md) | Deterministic async, runtime service, callback, and lifetime tests |
| [`test/concurrency-and-sanitizer.md`](test/concurrency-and-sanitizer.md) | Concurrency tags, race matrices, TSan gates, and review checklist |
| [`test/uimodel-and-gtk.md`](test/uimodel-and-gtk.md) | UIModel policy tests, GTK adapter tests, geometry, lifecycle, and testability |
| [`test/coverage-workflow.md`](test/coverage-workflow.md) | Coverage reports and the measure-analyze-test-verify loop |
| [`test/validation-and-review.md`](test/validation-and-review.md) | New files, reviewing existing tests, validation, smells, and review checklist |
| [`test/test-suite.md`](test/test-suite.md) | `./ao test` suite organization |
| [`test/performance.md`](test/performance.md) | Standalone optimized performance sampling and report evidence |

## Layer selection

Choose the lowest layer that proves the behavior:

- `lib`: pure algorithms, storage, parsing, serialization, query evaluation, audio primitives, and utility behavior.
- `runtime`: service contracts, runtime state transitions, projections, subscriptions, callbacks, and async lifecycle.
- `uimodel`: UI policy, view-state projection, menu models, editor models, validation, field formatting, and presentation decisions.
- Frontend layers (`linux-gtk`, `tui`, `cli`, and `winui`): construction or formatting, render binding, event/command routing, platform lifecycle, and targeted frontend regressions.

When choosing between `uimodel` and GTK, prefer `uimodel` if the behavior can be
expressed as input state to view state. GTK tests should stay thin and prove only
adapter behavior.

For detailed layer selection guidance with boundary-case tables, see
`doc/development/test/layer-selection.md`.

## Bazel support tests

Python tests of the Bazel support layer own its generators, integration mechanisms,
and specific failure boundaries, not verification of the entire product build.
Test generated consumer interfaces, argument forwarding, environment isolation,
host/target and configuration selection, input invalidation, and failure propagation
with the smallest representative fixtures that distinguish correct behavior from a
plausible defect.

Do not maintain a second dependency, source, or target inventory in expected values,
reconstruct the complete build graph, or multiply every dependency across every
platform and configuration without a distinct behavioral reason.
A source/target assertion needs an independently required ownership or consumer
boundary; the current BUILD syntax or list order is not itself a contract.
Literal expected values are appropriate for transformations and required consumer
outputs, but copying configuration or the production algorithm is not an independent
oracle. Compare exact generated text only when its spelling is an externally
consumed format contract.

Keep production selection and validation logic real; fake external I/O at its
boundary. A subprocess fake must discriminate the executable, query, and module
relevant to the tested contract rather than return a valid answer for any command.
Use a small number of real integration probes to establish that Bazel consumes the
generated interfaces. Actual build, compiler, linker, and runtime acceptance belongs
to the affected native build/test gates, not Python assertions about dummy artifacts.
This boundary does not exclude C++ business tests from `./ao bazel test` or weaken
required [completion validation](test/validation-and-review.md).

## Naming and tags

Name tests as behavior contracts: `"Component - behavior under condition"`.
An ordinary case takes no tag; the only retained markers are `[concurrency]`
for cross-thread contracts and `[.manual]` for real-hardware or deliberate
manual execution.

For naming examples, the two-marker contract, and anti-patterns, see
`doc/development/test/naming-and-assertion.md`.

## Assertion quality

Every test should assert observable outcomes: return values, persisted state,
emitted signals, callback payloads, or rendered view state. Mutation tests must
assert postconditions through the public API.

For assertion patterns and `REQUIRE` vs `CHECK` guidance, see
`doc/development/test/naming-and-assertion.md`.

## Fixtures and helpers

Helpers should remove repetitive setup, not hide the behavior under test.
Search existing `*TestSupport.h` files before creating new shared helpers.
Prefer explicit expected values over duplicating production algorithms.
For testability seam rules, see
`doc/development/test/fixture-and-helper.md#testability-seams`.

For helper file naming, use `doc/development/naming-convention.md`.

For fixture patterns and FakeIt guidance, see
`doc/development/test/fixture-and-helper.md`.

## Runtime and asynchronous tests

Use deterministic executors, barriers, and explicit callback progression instead
of sleep/yield loops. Assert callback payloads and non-emission, not just
`called == true`.

For async test patterns and executor examples, see
`doc/development/test/runtime-and-async.md`.

## UIModel and GTK tests

UI policy belongs in `uimodel` whenever possible. GTK tests verify adapter
behavior: widget binding, event routing, lifecycle cleanup, and targeted geometry
invariants.

For GTK-specific rules including the "Three Questions" framework, geometry
tests, lifecycle rules, and testability advice, see
`doc/development/test/uimodel-and-gtk.md`.

## Large test files

Split by behavior domain, not by line count. Prefer separate `TEST_CASE`s for
independent contracts; use `SECTION` for variants sharing one arrange.

For split guidelines and the review checklist, see
`doc/development/test/validation-and-review.md`.

## Validation

Use [validation and review](test/validation-and-review.md) for the change-specific
completion route, native host coverage, result reuse, and lint boundaries.
That document is the single completion authority; focused checks during
implementation supplement any full gate it requires.

For coverage-specific workflow, see `doc/development/test/coverage-workflow.md`.
