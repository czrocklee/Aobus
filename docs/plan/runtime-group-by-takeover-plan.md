# Runtime Group-By Projection Takeover Plan

## Scope

This plan describes how to make the runtime projection fully own track-list group-by behavior while keeping GTK responsible only for rendering widgets.

The target behavior is not merely to move one helper such as `groupLabelFor()` into runtime. The target is that runtime owns the complete group-by contract:

1. the selected group key,
2. the effective ordering required by that group key,
3. group identity and section boundaries,
4. group header labels,
5. redundant-field hints used by the UI to hide columns, and
6. projection notifications when any of the above changes.

GTK should render dropdowns, columns, headers, and rows, but it should not decide group semantics.

## Current State

Runtime already has partial group-by state:

- `ao::app::TrackGroupKey` is defined in `app/runtime/StateTypes.h`.
- `TrackListViewState` and `TrackListViewConfig` carry `groupBy`.
- `ViewService::setGrouping()` updates runtime view state and publishes `ViewGroupingChanged`.
- `TrackListProjection` owns ordered track IDs and projection deltas.

However, the ownership boundary is still split:

- `app/linux-gtk/ui/TrackPresentation.*` owns the GTK-specific `TrackGroupBy` enum.
- GTK owns `presentationSpecForGroup()`, which derives the sort preset for each grouping mode.
- GTK owns `groupLabelFor()`, which formats group headers.
- GTK owns `shouldShowColumn()`, which suppresses redundant metadata columns.
- `TrackViewPage::applyPresentationSpec()` pushes both sort and grouping into runtime, making GTK the de facto source of truth for group presets.
- `TrackListProjection` does not expose group labels, group identity, group ranges, or redundant fields.
- `TrackListAdapter` consumes projection deltas only as ordered track IDs.

There is also a wiring detail to verify during implementation: `TrackViewPage` builds a `_sectionHeaderFactory`, but the current code search does not show it being connected to a GTK section/header API. The takeover should include a real section rendering path instead of only moving label formatting.

## Target End State

```diagram
╭────────────────────╮
│ Group dropdown     │
│ GTK widget only    │
╰─────────┬──────────╯
          │ selected TrackGroupKey
          ▼
╭────────────────────╮
│ ViewService        │
│ canonical view     │
│ state owner        │
╰─────────┬──────────╯
          │ applies runtime presentation policy
          ▼
╭────────────────────╮
│ TrackListProjection│
│ order + groups +   │
│ labels + hints     │
╰─────────┬──────────╯
          │ projection deltas and presentation snapshot
          ▼
╭────────────────────╮
│ TrackListAdapter   │
│ materializes rows  │
│ and forwards group │
│ metadata           │
╰─────────┬──────────╯
          │ GTK objects, no grouping decisions
          ▼
╭────────────────────╮
│ TrackViewPage      │
│ renders columns,   │
│ headers, selection │
╰────────────────────╯
```

Responsibilities after the migration:

### Runtime Owns

- `TrackGroupKey` as the only group-by enum used by application logic.
- The canonical mapping from `TrackGroupKey` to effective `TrackSortTerm` list.
- The canonical mapping from `TrackGroupKey` to redundant metadata fields.
- Group comparison semantics.
- Group label formatting for the current projection.
- Group range or same-group metadata sufficient for GTK to render section headers.
- Reapplying grouping after source/projection recreation.

### GTK Owns

- The group dropdown widget and static option labels.
- Static column definitions, column widths, order, visibility toggles, and persistence.
- Mapping runtime metadata fields to GTK columns.
- Rendering section/header widgets and row widgets from runtime-provided data.
- User interaction forwarding: when the dropdown changes, GTK calls `ViewService::setGrouping()`.

GTK must not own group sort presets, group labels, or group identity.

## Behavioral Compatibility Rules

The migration should preserve current user-facing group modes:

| Group key | Effective sort terms | Redundant fields for column hiding | Header label |
|-----------|----------------------|------------------------------------|--------------|
| `None` | no forced group preset | none | none |
| `Artist` | Artist, Album, DiscNumber, TrackNumber, Title | Artist | artist or `Unknown Artist` |
| `Album` | AlbumArtist, Album, DiscNumber, TrackNumber, Title | Artist, Album, AlbumArtist | `Album - Album Artist`, album only when album artist is empty, or `Unknown Album` |
| `AlbumArtist` | AlbumArtist, Album, DiscNumber, TrackNumber, Title | AlbumArtist | album artist or `Unknown Album Artist` |
| `Genre` | Genre, Artist, Album, DiscNumber, TrackNumber, Title | Genre | genre or `Unknown Genre` |
| `Composer` | Composer, Artist, Album, DiscNumber, TrackNumber, Title | Composer | composer or `Unknown Composer` |
| `Work` | Work, Artist, Album, DiscNumber, TrackNumber, Title | Work | work or `Unknown Work` |
| `Year` | Year, Artist, Album, DiscNumber, TrackNumber, Title | Year | year or `Unknown Year` |

Before moving ownership, explicitly choose the canonical comparison semantics and lock them down with runtime tests. Current GTK helper code and runtime projection sorting are not identical:

- GTK text comparison treats empty text as unknown and places unknown values after known values.
- GTK numeric comparison treats `0` as unknown and places unknown values after known values.
- GTK comparison falls back to track ID for deterministic ordering.
- Runtime currently normalizes text by lowercasing and stripping leading English articles, and it does not consistently apply an explicit track-ID tie-breaker.

Because the bound track list already receives row order from `TrackListProjection`, the canonical behavior should be defined as the intended current visible behavior, not blindly copied from either side. The implementation PR must state the chosen semantics and add tests for them before deleting GTK helpers.

## Proposed Runtime API Shape

Keep the API small and projection-centered. A concrete implementation can adjust names, but it should expose these concepts from `app/runtime/ProjectionTypes.h` or a runtime-local companion header.

```cpp
struct TrackListPresentationSnapshot final
{
  TrackGroupKey groupBy = TrackGroupKey::None;
  std::vector<TrackSortTerm> effectiveSortBy{};
  std::vector<TrackSortField> redundantFields{};
  std::uint64_t revision = 0;
};

struct TrackGroupSectionSnapshot final
{
  Range rows{};
  std::string label{};
};
```

Extend `ITrackListProjection` with presentation/group queries. The exact surface can be row-index based, track-ID based, or both, depending on GTK section-model needs. The important rule is that GTK asks runtime instead of recomputing.

Recommended minimum:

```cpp
class ITrackListProjection
{
public:
  virtual TrackListPresentationSnapshot presentation() const = 0;
  virtual std::size_t groupCount() const noexcept = 0;
  virtual TrackGroupSectionSnapshot groupAt(std::size_t groupIndex) const = 0;
  virtual std::optional<std::size_t> groupIndexAt(std::size_t rowIndex) const = 0;
};
```

GTK-friendly forwarding helpers may also be useful:

```cpp
virtual std::string groupLabelAt(std::size_t rowIndex) const = 0;
virtual bool sameGroupAt(std::size_t lhsRowIndex, std::size_t rhsRowIndex) const = 0;
```

If GTK header binding only has access to a `TrackRow`, `TrackListAdapter` can maintain a small row-index lookup while applying projection deltas, or the projection can additionally expose `groupLabelFor(TrackId)` and `sameGroup(TrackId, TrackId)`. Do not let GTK rebuild group keys from row metadata.

## Implementation Slices

The slices below are ordered so each step remains buildable and testable.

## Slice 1: Add Runtime Presentation Policy

### Objective

Introduce the canonical runtime mapping from `TrackGroupKey` to presentation policy without changing GTK behavior yet.

### Changes

- Add `TrackListPresentationSnapshot` and, if needed, `TrackGroupSectionSnapshot` to runtime projection types.
- Add a runtime helper such as `presentationForGroup(TrackGroupKey)`.
- Put the helper near runtime projection code first if that keeps the change smaller. A separate `TrackListPresentation.*` runtime file is acceptable if it avoids crowding `TrackListProjection.cpp`.
- The helper returns:
  - `groupBy`,
  - `effectiveSortBy`, and
  - `redundantFields`.
- Use `ao::app::TrackSortField` for `redundantFields`; do not introduce `TrackColumn` into runtime.

### Tests

- Add runtime tests for each group key mapping to effective sort terms.
- Add runtime tests for redundant fields per group key.
- Keep existing GTK tests unchanged in this slice.

### Checkpoint

Runtime can describe group presentation policy, but GTK still renders with the old helpers.

## Slice 2: Make ViewService Authoritative For Group-To-Sort

### Objective

Make `ViewService` the only owner of group-to-sort application. GTK should stop deriving and pushing group sort presets.

### Changes

- Add a private helper in `ViewService.cpp`, conceptually `applyPresentation(ViewEntry&)`, that:
  1. derives presentation policy from `entry.state.groupBy`,
  2. updates `entry.state.sortBy` to the policy's effective sort terms,
  3. configures the concrete `TrackListProjection`, and
  4. avoids duplicate rebuilds.
- Use this helper wherever projections are created or recreated:
  - `createView()`,
  - `setGrouping()`,
  - `setFilter()` when it creates a new projection,
  - `openListInView()` when it creates a new projection.
- Prefer a single projection mutator such as `TrackListProjection::setPresentation(TrackGroupKey, std::vector<TrackSortTerm>)` over separate `setSortBy()` plus grouping calls.
- `ViewService::setGrouping()` should:
  - no-op if the group key is unchanged,
  - update `state.groupBy`,
  - update `state.sortBy` from the runtime policy,
  - increment state revision,
  - apply presentation to the projection,
  - publish `ViewGroupingChanged`, and
  - publish `ViewSortChanged` only if any existing consumer still depends on that event.
- `ViewService::setSort()` should remain available for explicit user sorting, but group presets should not be computed in GTK. If manual sort while grouped is not supported yet, document that `setGrouping()` owns the active sort preset.
- `TrackViewPage` should call only `ViewService::setGrouping()` when the dropdown changes.

### Constructor Guardrail

`TrackViewPage` currently initializes `_presentationSpec` to `None` and calls `applyPresentationSpec()`, which can write `None` back into runtime on page construction. After this slice:

1. page construction must read the initial runtime view state or projection presentation,
2. set the dropdown selection from that runtime state,
3. apply column visibility from runtime presentation, and
4. connect the dropdown change signal only after hydration.

This prevents restored or preconfigured views from being overwritten by a default UI value.

### Tests

- `ViewService` creates a view with `groupBy != None` and stores the derived effective sort.
- `ViewService::setGrouping()` updates both `state.groupBy` and `state.sortBy`.
- Recreated projections after `setFilter()` and `openListInView()` preserve group presentation.
- Setting the same group twice does not publish unnecessary resets/events if the implementation chooses to optimize no-ops.

### Checkpoint

Runtime owns the write path for group presets. GTK still may render labels/columns using old helpers, but it no longer decides effective group sorting.

## Slice 3: Teach TrackListProjection Group Metadata

### Objective

Make `TrackListProjection` own group identity, group ranges, and labels for the current ordered projection.

### Changes

- Extend projection internals with the active `TrackGroupKey` and current presentation snapshot.
- Extend `OrderEntry` or an adjacent internal structure with group metadata.
- Build group descriptors while rebuilding the order index.
- Build group sections after sorting so section ranges are contiguous in projection order.
- Implement the new projection queries:
  - presentation snapshot,
  - group count,
  - group section by index,
  - group index or label for row index,
  - same-group checks if needed by GTK.
- Make load-mode computation account for both sorting and grouping fields. For example, grouping by `Work` needs cold metadata even if the sort list is later optimized.
- Keep group labels in runtime for now as plain English strings matching current UI labels. If localization is added later, convert these to structured label IDs instead of moving label semantics back to GTK.

### Incremental Update Strategy

Prefer correctness over clever deltas in the first implementation:

- On group key changes, emit `ProjectionReset`.
- On batch insert/update/remove callbacks, rebuild group sections and emit `ProjectionReset` if group boundaries may have changed.
- For single-row updates, it is acceptable to emit `ProjectionReset` when the row's group key changes. Fine-grained remove/insert/update can be restored later after group-section tests are stable.

### Semantic Decisions To Lock Down

Add tests before finalizing these details:

- unknown text ordering,
- unknown numeric ordering,
- case-insensitive text comparison,
- leading-article normalization or no normalization,
- deterministic track-ID tie-breaker,
- album group identity: album plus album artist, not album title alone,
- year `0` identity and label.

### Tests

- Group labels for every group key, including unknown values.
- Group section ranges for sorted data.
- Album groups split by album artist when album titles match.
- Same-group behavior for rows in the same and different groups.
- Insert into the middle of a group keeps sections correct.
- Removing the first row of a group updates or removes the section.
- Updating a track so it moves to another group updates order and sections.
- Batch update callbacks produce a correct projection after reset.

### Checkpoint

The projection has enough information for GTK to render grouping without inspecting row metadata.

## Slice 4: Switch GTK To Runtime Group Metadata

### Objective

Make GTK a pure consumer of runtime grouping data.

### Changes

- Replace GTK's `TrackGroupBy` usage in `TrackViewPage` with `ao::app::TrackGroupKey`.
- Keep only dropdown-position mapping in GTK:
  - dropdown index to `TrackGroupKey`,
  - `TrackGroupKey` to dropdown index,
  - static human-readable dropdown option text.
- Remove `TrackPresentationSpec` from `TrackViewPage`.
- Replace `applyPresentationSpec()` with runtime hydration and a small `applyRuntimePresentation()` that only updates UI widgets.
- Add `TrackListAdapter` forwarding helpers for projection presentation/group queries, or expose the bound projection through a narrow const accessor.
- Header rendering should ask runtime for the group label.
- Section boundary detection should ask runtime for group ranges or same-group checks.
- Column visibility should use runtime redundant-field hints:
  - keep user column visibility from `TrackColumnLayoutModel`,
  - map `ao::app::TrackSortField` to GTK `TrackColumn`, and
  - hide a column when it is both present in `redundantFields` and not required for non-group rendering.
- Verify and wire the actual GTK header/section API. If GTK cannot consume runtime group sections directly, use a minimal GTK delegate that only calls runtime `sameGroup` or group-index queries. Do not reimplement comparisons or labels in GTK.

### GTK Code That Should Remain

- `TrackColumnDefinition`.
- `TrackColumnLayout` and normalization.
- Column width/order/visibility persistence.
- Cell factories and inline editing widgets.
- The group dropdown widget itself.

### GTK Code That Should Disappear Or Stop Owning Semantics

- `TrackGroupBy`.
- `TrackPresentationSpec`.
- `presentationSpecForGroup()`.
- `compareForGrouping()`.
- `groupLabelFor()`.
- `shouldShowColumn()` as a semantic source of truth.

If a small GTK helper remains for mapping `TrackSortField` to `TrackColumn`, it must be named and tested as a mapping layer, not as group policy.

### Tests

- Move group policy tests from `test/unit/linux/TrackPresentationTest.cpp` to runtime tests.
- Keep GTK tests for column layout normalization and static column definitions.
- Add a focused GTK-side test for runtime redundant field to GTK column mapping if that helper is non-trivial.

### Checkpoint

GTK renders group-by state from runtime projection data and no longer computes group semantics.

## Slice 5: Cleanup And Persistence Follow-Through

### Objective

Remove split-brain code and make restored/preconfigured views keep their runtime grouping.

### Changes

- Delete dead GTK group policy helpers after all call sites move.
- Update includes and CMake sources if files are removed or new runtime files are added.
- Ensure `TrackListViewConfig{.groupBy = ...}` flows through `WorkspaceService::restoreSession()` and `ViewService::createView()` correctly.
- Existing GTK `SessionPersistence::saveSnapshot()` currently does not persist `openViews`; this is broader than group-by. Do not expand this migration just to redesign session persistence, but add or keep tests against an in-memory `ISessionPersistence` so runtime restore semantics are correct.
- If session view persistence is implemented in the same PR series, include `groupBy` and avoid restoring from any GTK enum.

### Tests

- Headless/runtime session restore with grouped open views.
- Restored grouped view does not get overwritten to `None` when `TrackViewPage` is constructed.
- No references remain to GTK `TrackGroupBy` outside deleted compatibility shims.

### Checkpoint

There is one group-by source of truth: runtime state and runtime projection.

## Test Coverage Plan

### Runtime Projection Tests

Add coverage in `test/unit/app/TrackListProjectionTest.cpp` or a dedicated runtime presentation test file for:

- `presentationForGroup()` mapping for all group keys.
- Effective sort terms per group key.
- Redundant fields per group key.
- Group labels for all group keys.
- Unknown label handling.
- Group sections and row ranges.
- Group updates after insert, remove, update, reset, and batch callbacks.
- Deterministic ordering under equal group/sort keys.
- Load-mode coverage for cold metadata group fields such as `Work`.

### View Service Tests

Add or extend `test/unit/app/ViewServiceTest.cpp` for:

- `createView()` with initial `groupBy` applies runtime effective sort to state and projection.
- `setGrouping()` updates state and projection in one path.
- `setFilter()` projection recreation preserves group presentation.
- `openListInView()` projection recreation preserves group presentation.
- `ViewGroupingChanged` is published with the selected key.

### GTK Tests

Keep GTK tests limited to GTK responsibilities:

- static column definitions,
- column layout normalization,
- runtime redundant-field to GTK column mapping,
- dropdown index to `TrackGroupKey` mapping if exposed in a testable helper.

Do not keep GTK tests that assert group sort presets, labels, or group identity after runtime takes over those semantics.

## Verification Commands

For focused implementation PRs, prefer the narrowest checks that cover touched code:

```bash
nix-shell --run "cmake --build /tmp/build/debug --target ao_test --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test [app][runtime][projection]"
nix-shell --run "/tmp/build/debug/test/ao_test [app][runtime][view]"
nix-shell --run "/tmp/build/debug/test/ao_test [app][presentation]"
```

If CMake has not been configured in `/tmp/build/debug`, use the repository's normal debug build path:

```bash
./build.sh debug
```

For changes touching GTK compilation or CMake source lists, run the full debug build and test suite.

## Risks And Guardrails

### Risk 1: GTK Remains The Hidden Source Of Truth

Guardrail: delete or de-semanticize `TrackGroupBy`, `presentationSpecForGroup()`, `groupLabelFor()`, `compareForGrouping()`, and `shouldShowColumn()` after migration. Any remaining GTK helper must only map runtime values to GTK widgets.

### Risk 2: Page Construction Overwrites Runtime State

Guardrail: hydrate `TrackViewPage` from runtime before connecting dropdown signals. Construction must not call `setGrouping(None)` or `setSort({})` as a default write-back.

### Risk 3: Projection Recreation Loses Grouping

Guardrail: centralize projection presentation application in `ViewService` and call it from every projection creation/recreation path.

### Risk 4: Runtime Leaks GTK Types

Guardrail: runtime exposes `TrackGroupKey`, `TrackSortField`, group sections, and labels. It must not include GTK headers or expose `TrackColumn`.

### Risk 5: Fine-Grained Group Deltas Become Fragile

Guardrail: allow `ProjectionReset` for group changes and group-boundary-changing mutations in the first pass. Optimize later only after section tests are stable.

### Risk 6: Stale Projection References In GTK

Guardrail: when `ViewService` swaps projection instances after filter or list changes, ensure `TrackPageGraph` or `TrackListAdapter` can rebind, or prefer preserving projection identity and swapping its source/config internally. The plan should not add new projection queries to GTK without addressing this path.

### Risk 7: Quick Filter Interactions Distract From Group-By Ownership

Guardrail: the existing quick filter remains out of scope unless it blocks grouped projection rendering. Do not let the adapter filter path compute group labels, group identity, or group sorting.

## Non-Goals

- Redesigning track-list visuals.
- Replacing quick filter behavior with a fully runtime-backed query-view model.
- Adding synthetic group header rows unless GTK section rendering cannot consume runtime group metadata.
- Introducing localization infrastructure for group labels.
- Supporting independent manual column sorting while grouped, unless already required by another active task.
- Reworking session persistence beyond preserving runtime `TrackListViewConfig::groupBy` where view snapshots already exist.

## Acceptance Criteria

The migration is complete when all of these are true:

- `TrackViewPage` no longer computes group sort presets.
- `TrackViewPage` no longer formats group labels from row metadata.
- `TrackViewPage` no longer decides group-based column hiding except by mapping runtime redundant fields to GTK columns.
- `TrackListProjection` exposes current group presentation metadata.
- `ViewService::setGrouping()` applies the effective runtime presentation to state and projection.
- Projection recreation paths preserve grouping.
- Runtime tests cover group policy, labels, group sections, and mutation behavior.
- GTK tests cover only GTK mapping/layout behavior.
- Dead GTK group-policy helpers are deleted or reduced to non-semantic widget mapping.

## Recommended Pull Request Order

1. Runtime presentation policy and tests.
2. `ViewService` group-to-sort ownership and constructor hydration guardrail.
3. `TrackListProjection` group metadata and section tests.
4. GTK consumption of runtime group metadata.
5. Cleanup of dead GTK group policy and persistence follow-through tests.

This order keeps each PR understandable and prevents a temporary state where GTK and runtime both claim ownership of group-by semantics.
