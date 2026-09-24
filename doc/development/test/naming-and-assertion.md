---
id: development.test.naming-and-assertion
---
# Test naming, tags, and assertions

## Naming

Prefer:

```cpp
TEST_CASE("Component - behavior under condition", "[layer][scope][component]")
```

Function-level tests may use:

```cpp
TEST_CASE("functionName returns result under condition", "[layer][unit][component]")
```

Examples:

```cpp
TEST_CASE("NotificationService - unchanged keyed report does not publish an update",
          "[runtime][unit][notification]")

TEST_CASE("ActivityStatusFeedProjection - pinned info remains in detail after compact hides",
          "[uimodel][unit][activity-status]")

TEST_CASE("TrackPresentationButton - selecting preset updates active list presentation",
          "[gtk][integration][track-presentation]")

TEST_CASE("TrackFieldGrid - collapsed metadata keeps custom rows hidden",
          "[gtk][unit][track-field-grid]")
```

Avoid vague names such as `"ActionRegistry"`, `"Library Export/Import Cycle"`, or `"Simple Equal Match"` for new tests. Legacy tests may exist; do not copy weak naming.

## Tags

Every case starts with `[layer][scope][component]`:

- **Layer** is the testing layer from [layer selection](layer-selection.md):
  the owning module for `lib` tests (`[core]`, `[library]`, `[query]`, `[audio]`,
  `[media]`, `[lmdb]`, or `[utility]`), otherwise `[runtime]`, `[uimodel]`,
  `[gtk]`, `[winui]`, `[tui]`, `[cli]`, or `[perf]`.
- **Scope** is exactly one of `[unit]` or `[integration]`; see
  [scope and metadata](layer-selection.md#scope-and-independent-metadata).
- **Component** names the domain, such as `[track-store]`, `[serializer]`, or
  `[import-export]`. A scope, descriptive, or hidden tag cannot stand in for it.

Further tags are optional and need a real selection use:

- Descriptive markers such as `[regression]`, `[workflow]`, or `[smoke]` are
  retired: they select nothing, and the test name states the protected behavior.
- `[concurrency]` marks the contracts in
  [concurrency and sanitizer validation](concurrency-and-sanitizer.md) and
  selects the `--concurrency` gate.
- `[async]` marks owner-thread asynchronous ordering and lifetime contracts
  from the same document; it is also the component of the async runtime's own
  tests.
- `[stress]` marks deliberate repetition and requires `[concurrency]`.
- A hidden tag such as `[.manual]` opts out of automatic runs. Reserve it for
  real hardware or deliberate manual execution; `[integration]` does not imply it.

```cpp
"[runtime][unit][async][concurrency][stress]"
"[runtime][integration][import-export][yaml]"
```

Use singular kebab-case names (`[track-store]`, `[preference]`), but keep proper
names and literal identifiers such as `[windows]` or the CLI `[stats]` command.
Avoid duplicate and synonymous tags.

`./ao test-audit` checks the prefix, the single scope, spelling, duplicates,
`[stress]` pairing, and retired tags; `./ao hygiene` enforces it for changed test files. It cannot
judge whether the chosen scope matches the contract, and it also accepts `tag`
as a layer.

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
CHECK(entry.lifetime == NotificationLifetime::pinned());
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
