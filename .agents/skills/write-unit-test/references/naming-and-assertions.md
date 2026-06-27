# Naming, tags, and assertions reference

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

TEST_CASE("ActivityStatusModel - detail-only notification does not replace compact state",
          "[uimodel][unit][activity-status]")

TEST_CASE("TrackPresentationButton - selecting preset updates active list presentation",
          "[gtk][unit][track-presentation]")

TEST_CASE("TrackFieldGrid - collapsed custom section keeps value column stable",
          "[gtk][regression][track-field-grid]")
```

Avoid vague names such as `"ActionRegistry"`, `"Library Export/Import Cycle"`, or `"Simple Equal Match"` for new tests. Legacy tests may exist; do not copy weak naming.

## Tags

Use tags that identify layer, test type, and component/domain.

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

## Test structure

1. Arrange only the state needed by the behavior.
2. Act once, unless the contract is specifically about repeated calls, ordering, or idempotence.
3. Assert observable outcomes and postconditions.
4. Use `SECTION` when the setup genuinely belongs to every section.
5. Keep one clear scenario per section.
6. Prefer local helpers over hidden global state.
7. Delete commented-out assertions and stale notes.

## Assertion quality

Weak assertions:

```cpp
REQUIRE(result);
CHECK(optView.has_value());
CHECK(count == 3);
```

Better assertions:

```cpp
REQUIRE(result);
CHECK(result->id == expectedId);
CHECK(result->state == ExpectedState::Ready);

REQUIRE(optView.has_value());
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
REQUIRE(optView.has_value());
CHECK(optView->metadata().title() == "After");
```

Use `CHECK` for independent observations after the action:

```cpp
CHECK(entry.id == id);
CHECK(entry.message == "Importing library");
CHECK(entry.sticky);
```

## Expected values

Prefer explicit known examples over recomputing expected values with production-like algorithms. A test that duplicates the production algorithm can pass even when both are wrong.
