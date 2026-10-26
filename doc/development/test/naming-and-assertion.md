---
id: development.test.naming-and-assertion
---
# Test naming, tags, and assertions

## Naming

Prefer:

```cpp
TEST_CASE("Component - behavior under condition")
```

Function-level tests may use:

```cpp
TEST_CASE("functionName returns result under condition")
```

Examples:

```cpp
TEST_CASE("NotificationService - unchanged keyed report does not publish an update")

TEST_CASE("ActivityStatusFeedProjection - pinned info remains in detail after compact hides")

TEST_CASE("TrackPresentationButton - selecting preset updates active list presentation")

TEST_CASE("TrackFieldGrid - collapsed metadata keeps custom rows hidden")
```

Avoid vague names such as `"ActionRegistry"`, `"Library Export/Import Cycle"`, or `"Simple Equal Match"` for new tests. Legacy tests may exist; do not copy weak naming.

## Tags

An ordinary case takes no tag at all: the name states the behavior, and the
[owning layer](layer-selection.md) is a design choice expressed by placement,
not by tags.

The only supported markers are:

- `[concurrency]` marks the contracts in
  [concurrency and sanitizer validation](concurrency-and-sanitizer.md) and
  selects the `--concurrency` gate.
- `[.manual]` hides a case from automatic runs. Reserve it for real hardware or
  deliberate manual execution whose prerequisites an automated run cannot
  provide.

A tagged case passes its markers as the second, literal argument:

```cpp
TEST_CASE("PipeWireMonitor - concurrent subscriptions retain distinct registrations",
          "[concurrency]")
```

Use name globs to select related cases.
Catch2 treats commas as OR separators even inside shell quotes. Escape every
literal comma as `\,` when selecting a name that contains commas:

```bash
./ao test --core --list "FieldCatalog - finds descriptors by canonical name\, alias\, and field"
```

`TEMPLATE_TEST_CASE` requires a tag argument: pass an empty string for an
untagged template case or a string containing the supported markers.

```cpp
TEMPLATE_TEST_CASE("FileTimeConversion - a UTC-scale file clock maps leap seconds onto the Unix scale",
                   "",
                   std::nano,
                   (std::ratio<1, 10'000'000>))
```

Use literal string arguments only. Registration arguments assembled from
macros, concatenation, or other expressions fail the audit as unsupported
syntax, as do invocations of unsupported public registration macro families.
Backslash-newline line splices are honored outside invocations, so a comment,
identifier, or literal continued by a splice is still recognized; a
registration invocation that itself contains a splice — in the macro name,
between the macro and its parenthesis, or in an argument — fails the audit
as unsupported syntax instead of being silently missed.

`./ao test-audit` discovers registrations with one content detector shared by
default, directory, explicit-file, and hygiene scopes. It scans the governed
test sources (the exact lowercase suffixes `.cpp`, `.mm`, `.h`, `.hpp`, and
`.def` under `test/`, excluding the lint checker fixtures under `test/lint/`);
this is the same case-sensitive suffix contract used by the other source tools,
not host-filesystem case folding. Uppercase suffixes such as `.CPP` and `.HPP`
are outside that governed universe; registration filenames must use the
conventional lowercase suffixes on every host. CMake rejects case-variant source
extensions such as `.CPP` in registered test targets so they cannot compile
outside this audit contract. In candidate files, comments, strings,
character literals, and custom raw strings outside invocations are not
registrations. The scan honors backslash-newline line splices for that
lexical detection — continued comments stay comments, and raw string bodies
keep their original spelling, so a splice cannot forge a closing delimiter. Registrations must live in `*Test.cpp`, `*Test.mm`, or
`*Benchmark.cpp` files. The source audit rejects registrations in headers,
include fragments, and other discovered filenames such as `FooSupport.cpp`.
`./ao test-audit` and the source audit in `./ao hygiene` check
registration syntax, names, markers, and placement without requiring a build
tree. After a successful build, `./ao check` reconciles whole-project discovery
with that build's fresh CMake-configured owner/disabled manifest. Only that
configured check establishes ownership completeness; standalone source audits
do not detect a missing CMake owner.

```bash
./ao test-audit test/query
./ao test-audit --fail-on-issue test/query/ParserTest.cpp
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
