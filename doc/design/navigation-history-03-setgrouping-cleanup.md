# `setGrouping` Cleanup Plan

## Problem

`ViewService` currently has two ways to describe presentation state:

- group-oriented API: `setGrouping(ViewId, TrackGroupKey)`
- presentation-oriented API: `setPresentation(ViewId, TrackPresentationSpec)`

Navigation history needs one canonical state shape. The canonical shape should
be `TrackPresentationSpec`, because it captures grouping, sort, visible fields,
and redundant fields.

## Cleanup Rule

Do not remove `setGrouping` until all runtime configuration and session paths
can preserve a complete presentation spec.

## Phase 1: Make TrackListViewConfig Presentation-Based

Change:

```cpp
struct TrackListViewConfig final
{
  ListId listId{};
  std::string filterExpression{};
  TrackGroupKey groupBy = TrackGroupKey::None;
  std::vector<TrackSortTerm> sortBy{};
};
```

to:

```cpp
struct TrackListViewConfig final
{
  ListId listId{};
  std::string filterExpression{};
  TrackPresentationSpec presentation{};
};
```

Compatibility handling:

- If decoded session data has an empty presentation id and legacy
  `groupBy/sortBy`, build a presentation from the closest built-in preset and
  overlay legacy sort terms.
- After save, write only the presentation-based shape.

If the current config serializer does not support versioned migrations, keep
temporary legacy fields in a session-only compatibility struct instead of
keeping them in the runtime view config.

## Phase 2: Create Views From Presentation

Update `ViewService::createView`:

```cpp
auto state = TrackListViewState{
  .id = id,
  .lifecycle = attached ? ViewLifecycleState::Attached
                        : ViewLifecycleState::Detached,
  .listId = initial.listId,
  .filterExpression = initial.filterExpression,
  .presentation = normalizeTrackPresentationSpec(initial.presentation),
};
state.groupBy = state.presentation.groupBy;
state.sortBy = state.presentation.sortBy;
```

Then call only:

```cpp
applyPresentation(entry, entry.state.presentation);
```

Remove the overload that infers a presentation from `groupBy`.

## Phase 3: Fix Sort State Consistency

`ViewService::setSort` must keep `state.presentation.sortBy` consistent:

```cpp
it->second.state.sortBy = sortBy;
it->second.state.presentation.sortBy = sortBy;
```

Then apply the full presentation to the projection. This prevents history and
session snapshots from losing manual sort changes.

## Phase 4: Route Album Jump Through Presentation

Replace:

```cpp
_runtime.views().setGrouping(viewId, rt::TrackGroupKey::Album);
```

with the workspace command:

```cpp
_runtime.workspace().jumpToAlbum(_currentTrackId);
```

`jumpToAlbum` should apply the built-in `"albums"` presentation by full spec.

## Phase 5: Remove Public Grouping API

Remove from `app/include/ao/rt/ViewService.h`:

- `GroupingChanged`
- `void setGrouping(ViewId, TrackGroupKey)`
- `Subscription onGroupingChanged(...)`

Remove from `app/rt/ViewService.cpp`:

- `groupingChangedSignal`
- `onGroupingChanged`
- `setGrouping`
- `applyPresentation(ViewEntry&)` overload that infers by group
- comments that mention direct `setGrouping`

Keep `TrackListViewState::groupBy` only if existing consumers need fast access.
It becomes derived state maintained by `setPresentation`, not an independent
mutation path.

## Phase 6: Update Tests

Remove group-only tests from `test/unit/runtime/ViewServiceTest.cpp`:

- `setGrouping updates state and projection`
- `setGrouping no-ops on same value`
- `setGrouping publishes ViewGroupingChanged`
- `setGrouping publishes PresentationChanged`
- `setGrouping no-ops does not publish event`

Replace with presentation-based tests:

- `createView applies initial presentation`
- `setPresentation updates group sort and projection`
- `setPresentation no-ops on identical normalized spec`
- `setSort updates presentation sort snapshot`
- `albums presentation replaces old jump-to-album grouping path`

Update `test/unit/runtime/HeadlessShellTest.cpp` cases that call
`runtime.views().setGrouping(...)` to use
`runtime.workspace().setActivePresentation(...)` or direct
`views.setPresentation(...)`, depending on whether the test is checking
workspace navigation or low-level view state.

## Phase 7: Compile-Time Cleanup Check

Add or keep a compile-level assertion by simply removing all references and
building the full test suite. A dedicated test named `no_setGrouping_symbol`
is not needed; the compiler provides the check.

Search commands before finishing:

```bash
rg -n "setGrouping|onGroupingChanged|GroupingChanged|groupingChangedSignal" app test
rg -n "TrackListViewConfig\\{[^}]*groupBy|\\.groupBy =|\\.sortBy =" app test
```

The second search should be reviewed manually because `TrackListViewState`
may still expose derived `groupBy` and `sortBy`.
