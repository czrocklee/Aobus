---
id: development.test.naming-and-assertion
type: development
status: current
domain: development
summary: Defines test names, tags, structure, assertions, and expected-value policy.
---
# Test naming, tags, and assertions

## Naming

Prefer:

```cpp
TEST_CASE("Component - behavior under condition", "[layer][type][component]")
```

Function-level tests may use:

```cpp
TEST_CASE("functionName returns result under condition", "[layer][unit][component]")
```

Examples:

```cpp
TEST_CASE("NotificationService - dismissing missing id does not publish event",
          "[runtime][unit][notification]")

TEST_CASE("ActivityStatusFeedProjection - detail-only notification does not replace compact state",
          "[uimodel][unit][activity-status]")

TEST_CASE("TrackPresentationButton - selecting preset updates active list presentation",
          "[gtk][unit][track-presentation]")

TEST_CASE("TrackFieldGrid - collapsed metadata keeps custom rows hidden",
          "[gtk][regression][track-field-grid]")
```

Avoid vague names such as `"ActionRegistry"`, `"Library Export/Import Cycle"`, or `"Simple Equal Match"` for new tests. Legacy tests may exist; do not copy weak naming.

## Tags

Use tags that identify layer, test type, and component/domain.

The testing layers from `layer-selection.md` map to first tags like this:

| Testing layer | First tag |
|---|---|
| `lib` | The owning module: `[core]`, `[library]`, `[query]`, `[audio]`, `[tag]`, `[media]`, `[lmdb]`, or `[utility]` |
| `runtime` | `[runtime]` |
| `uimodel` | `[uimodel]` |
| `linux-gtk` | `[gtk]` |
| TUI frontend | `[tui]` |
| CLI frontend | `[cli]` |
| council tool | `[council]` |
| performance baselines (`test/perf/`) | `[perf]` |

Recommended layer tags:

- `[core]`
- `[library]`
- `[query]`
- `[runtime]`
- `[uimodel]`
- `[gtk]`
- `[audio]`
- `[tag]`
- `[utility]`
- `[cli]`
- `[council]`
- `[lmdb]`
- `[media]`
- `[perf]`
- `[tui]`

Recommended type tags:

- `[unit]`
- `[workflow]`
- `[integration]`
- `[regression]`
- `[smoke]`

Recommended component/domain tags:

- `[notification]`
- `[activity-status]`
- `[track-store]`
- `[serializer]`
- `[plan-evaluator]`
- `[import-export]`

Cross-cutting behavior tags:

- `[concurrency]` for cross-thread access, synchronization, executor affinity,
  cancellation races, or teardown while work is in flight.
- `[stress]` only for deliberate repetition or schedule exploration. Pair it
  with `[concurrency]`; stress is an execution strategy, not a behavior domain.

Tag ordering: `[layer][type][subsystem]`. The layer tag is always first, the type
tag second, and optional subsystem tags follow. Keep the total at 3–4 tags. The
only five-tag form is `[layer][type][component][concurrency][stress]`, which
preserves the component while identifying a repeated race window. Put
`[concurrency]` last, or immediately before `[stress]`, when present. See
`concurrency-and-sanitizers.md` for the required contract matrix.

Use singular form for all tags: `[component]` not `[components]`, `[shortcut]`
not `[shortcuts]`.

Use kebab case for multi-word tags: `[track-store]` not `[track_store]` or
`[trackstore]`.

Catch2 opt-in integration drivers may use a hidden final tag with a leading
dot, such as `[.manual]`. The tag body must still use kebab case. Reserve hidden
tags for tests that require real hardware or deliberate manual execution; do
not hide ordinary unit or regression coverage.

Use the advisory audit when reviewing naming/tag cleanup or checking a focused
directory:

```bash
./ao test-audit test/unit/query
./ao test-audit --fail-on-issue test/unit/query/ParserTest.cpp
```

## Test structure

1. Arrange only the state needed by the behavior.
2. Act once, unless the contract is specifically about repeated calls, ordering, or idempotence.
3. Assert observable outcomes and postconditions.
4. Split independent contracts into separate `TEST_CASE`s; reserve `SECTION` for variants of one contract that share an arrange (see *SECTION vs TEST_CASE* below).
5. Prefer local helpers over hidden global state.
6. Reuse existing `*TestSupport.h` helpers before introducing new shared test plumbing.
7. Delete commented-out assertions and stale notes.

## SECTION vs TEST_CASE

`SECTION` shares one arrange and re-runs it fresh per section; a `TEST_CASE` isolates a
failure domain, so a crash or hard `REQUIRE` failure stops only that case. Decide by
failure isolation and setup divergence, not by scenario count:

- Separate `TEST_CASE`s when behaviors are independent contracts, when arrange steps
  have diverged, or when one failure would obscure the others.
- Keep `SECTION`s when variants share one arrange — typically the same constructed
  object under different inputs.

The two are coupled: promoting every section to a `TEST_CASE` duplicates the arrange,
which is what pushes setup into `*TestSupport.h`. Treat a sudden need for heavy new
support plumbing as a signal the split may not be warranted — split for failure domains,
not to hit a case count.

## Assertion quality

Weak assertions:

```cpp
REQUIRE(result);
CHECK(optView);
CHECK(count == 3);
```

Better assertions:

```cpp
REQUIRE(result);
CHECK(result->id == expectedId);
CHECK(result->state == ExpectedState::Ready);

REQUIRE(optView);
CHECK(optView->metadata().title() == "After");
CHECK(optView->property().duration() == std::chrono::minutes{3});

REQUIRE(items.size() == 3);
CHECK(items[0].id == firstId);
CHECK(items[1].id == secondId);
CHECK(items[2].id == thirdId);
```

For mutating APIs, always assert the resulting state through the public reader/API after the mutation commits or completes.

For callbacks, assert payloads, order, and non-emission when relevant. Do not only assert `called == true` unless the event itself is the whole behavior.

## REQUIRE vs CHECK

Use `REQUIRE` for preconditions that make later checks meaningless:

```cpp
auto optView = reader.get(id);
REQUIRE(optView);
CHECK(optView->metadata().title() == "After");
```

Use `CHECK` for independent observations after the action:

```cpp
CHECK(entry.id == id);
CHECK(entry.message == "Importing library");
CHECK(entry.lifetime == NotificationLifetime::untilDismissed());
```

## Expected values

Prefer explicit known examples over recomputing expected values with production-like algorithms. A test that duplicates the production algorithm can pass even when both are wrong.

Weak — recomputes the expectation with the same logic under test:

```cpp
// Both sides run trackPresentationComparator, so a wrong comparator still passes.
auto expected = tracks;
std::ranges::sort(expected, trackPresentationComparator(spec));
CHECK(projection.orderedTrackIds() == idsOf(expected));
```

Better — pin the literal contract, so a logic change is forced to update the test:

```cpp
auto const ids = projection.orderedTrackIds();
REQUIRE(ids.size() == 3);
CHECK(ids[0] == bachId);    // spec sorts: composer asc, then album, then track number
CHECK(ids[1] == mozartId);
CHECK(ids[2] == satieId);
```
