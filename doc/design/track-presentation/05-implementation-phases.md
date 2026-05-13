# Track Presentation Implementation Phases

## Purpose

This document turns the presentation preset design into staged engineering work. Each phase should leave the repository buildable and behaviorally coherent.

## Phase 0: Baseline

### Goal

Capture current behavior before replacing group-derived presentation logic.

### Changes

No code changes required unless adding characterization tests.

### Verification

```bash
./build.sh debug
```

### Acceptance Criteria

- Existing baseline is known.
- Any pre-existing failures are recorded.

## Phase 1: Runtime Types and Built-in Registry

### Goal

Introduce new runtime presentation data model without changing user-facing GTK behavior.

### Files

```text
app/runtime/StateTypes.h
app/runtime/TrackPresentationPreset.h
app/runtime/TrackPresentationPreset.cpp
app/runtime/CMakeLists.txt
test/unit/runtime/TrackPresentationPresetTest.cpp
```

### Changes

1. Add `TrackPresentationField`.
2. Add field id conversion helpers.
3. Add `TrackPresentationSpec`.
4. Add `TrackPresentationPreset`.
5. Add `builtinTrackPresentationPresets()`.
6. Add `defaultTrackPresentationSpec()`.
7. Add `normalizeTrackPresentationSpec()`.
8. Add tests for exact built-in definitions.
9. Keep old `presentationForGroup()` temporarily.

### Header Skeletons

See [Runtime Design](02-runtime-design.md).

### Verification

```bash
nix-shell --run "cmake --build /tmp/build/debug --target ao_test --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test [runtime]"
```

### Acceptance Criteria

- Built-in preset definitions are explicit and tested.
- Existing runtime view behavior still passes tests.

## Phase 2: ViewService Presentation State

### Goal

Make active presentation a runtime view-level concept.

### Files

```text
app/runtime/StateTypes.h
app/runtime/ViewService.h
app/runtime/ViewService.cpp
test/unit/runtime/ViewServiceTest.cpp
```

### Changes

1. Add `TrackListPresentationState`.
2. Add presentation state to `TrackListViewState` and `TrackListViewConfig`.
3. Add `ViewService::setPresentation(ViewId, TrackPresentationSpec const&)`.
4. Add `ViewService::setPresentation(ViewId, std::string_view id)`.
5. Add `PresentationChanged` event and subscription API.
6. Update view creation to apply default presentation.
7. Keep `setGrouping()` and `setSort()` as compatibility APIs.
8. Make compatibility APIs update presentation state consistently.

### Acceptance Criteria

- New views default to `songs` presentation.
- Switching presentation updates state revision.
- Switching presentation applies group/sort to the projection.
- Presentation event is emitted.
- Existing tests using `setGrouping()` and `setSort()` still pass during migration.

## Phase 3: Projection Snapshot Supports Presentation Fields

### Goal

Make active presentation visible through projection snapshot without materializing row values.

### Files

```text
app/runtime/ProjectionTypes.h
app/runtime/TrackListProjection.h
app/runtime/TrackListProjection.cpp
test/unit/runtime/TrackListProjectionTest.cpp
```

### Changes

1. Extend `TrackListPresentationSnapshot` with:
   - `presentationId`
   - `visibleFields`
   - `redundantFields`
2. Change `TrackListProjection::setPresentation()` to accept `TrackPresentationSpec`.
3. Keep comparator and section logic based only on group/sort.
4. Update tests.

### Do Not Do

- Do not add `valueFor(trackId, field)`.
- Do not add row snapshots to projection deltas.
- Do not include GTK headers.

### Acceptance Criteria

- Projection snapshot contains the active semantic presentation.
- Projection deltas remain range/track-id based.
- Sorting/grouping behavior remains correct.

## Phase 4: GTK Field-to-Column Adapter

### Goal

Allow GTK to turn runtime `visibleFields` into `TrackColumnLayout`.

### Files

```text
app/linux-gtk/track/TrackPresentation.h
app/linux-gtk/track/TrackPresentation.cpp
app/linux-gtk/track/TrackPresentationAdapter.h      # optional
app/linux-gtk/track/TrackPresentationAdapter.cpp    # optional
test/unit/linux-gtk/track/TrackPresentationTest.cpp
```

### Changes

1. Expose `trackColumnDefinition(TrackColumn)` if needed.
2. Expose `defaultTrackColumnState(TrackColumn)` if needed.
3. Add `trackColumnForPresentationField()`.
4. Add `trackColumnLayoutForPresentation()`.
5. Add tests for built-in layout mapping.

### Acceptance Criteria

- `albums` maps to Track, Title, Duration, Year, Tags.
- `classical-composers` maps to Work, Title, Artist, Album, Duration, Year.
- Redundant fields are hidden according to spec.
- GTK row loading code is unchanged.

## Phase 5: GTK Toolbar Preset Selector

### Goal

Replace main-toolbar group/columns controls with a presentation selector.

### Files

```text
app/linux-gtk/track/TrackViewPage.h
app/linux-gtk/track/TrackViewPage.cpp
```

### Changes

1. Remove `_groupByLabel`, `_groupByDropdown`, `_groupByOptions`, and `onGroupByChanged()`.
2. Add `_presentationButton` (`Gtk::MenuButton`), `_presentationPopover`, `_presentationMenuBox`, and `_presentationIds`.
3. Populate built-in presets into the popover menu.
4. On menu item activation, call `ViewService::setPresentation()`.
5. Stop appending `_columnController->columnsButton()` to the main toolbar.
6. Apply runtime presentation to `_columnLayoutModel`.
7. Keep `TrackColumnController` internally for actual column widgets.

### Acceptance Criteria

- Main toolbar shows filter + view selector.
- Main toolbar no longer shows group-by or columns controls.
- Selecting a view changes grouping, sorting, and columns together.
- Filter, playback activation, selection, tag editing, and inline editing continue to work.

## Phase 6: Remove Old Group-derived Presentation

### Goal

Delete the primitive group-only presentation policy.

### Files

```text
app/runtime/TrackListPresentation.h/.cpp    # delete or replace
test/unit/runtime/TrackListPresentationTest.cpp
app/runtime/CMakeLists.txt
```

### Changes

1. Remove `presentationForGroup()`.
2. Remove old tests that assert group-derived sort policy.
3. Update remaining call sites to use preset specs.
4. Rename old files if useful:
   - `TrackListPresentation.*` -> `TrackPresentationPreset.*`, or
   - delete old files if new files already exist.

### Acceptance Criteria

- No runtime code derives sort solely from group-by.
- Preset registry is the only built-in presentation source.
- Full debug build passes.

## Phase 7: Custom View Store and Persistence

### Goal

Persist custom view definitions and expose them to the selector.

### Files

```text
app/linux-gtk/app/UIState.h
app/linux-gtk/app/WindowController.cpp
app/linux-gtk/track/TrackPresentationStore.h/.cpp      # optional new GTK/app store
test/unit/linux-gtk/track/TrackPresentationStoreTest.cpp
```

### Changes

1. Add custom presentation state structs.
2. Add serialization/deserialization conversion.
3. Add validation and fallback rules.
4. Add a store that resolves built-in or custom id to `TrackPresentationSpec`.
5. Populate selector with custom views.
6. Persist active presentation id if appropriate.

### Acceptance Criteria

- Custom definitions survive restart.
- Invalid definitions do not crash startup.
- Unknown active presentation falls back to `songs`.

## Phase 8: Custom View Editor

### Goal

Add a user-facing editor for custom views.

### Files

```text
app/linux-gtk/track/TrackCustomViewDialog.h
app/linux-gtk/track/TrackCustomViewDialog.cpp
app/linux-gtk/track/TrackViewPage.cpp
app/linux-gtk/CMakeLists.txt
```

### Changes

1. Add dialog skeleton.
2. Support duplicating current built-in/custom presentation.
3. Support editing label.
4. Support group field dropdown.
5. Support sort term list.
6. Support visible field list.
7. Save through custom view store.
8. Refresh selector after save/delete.

### Acceptance Criteria

- User can create a custom view from Albums.
- User can change fields and save.
- User can switch away and back.
- User can delete custom view.

## Phase 9: Optional GTK Loading Optimization

### Goal

Only if profiling shows a problem, make GTK row cache aware of requested fields.

### Files

```text
app/linux-gtk/track/TrackRowCache.h/.cpp
app/linux-gtk/track/TrackRowObject.h/.cpp
```

### Changes

Possible future API:

```cpp
Glib::RefPtr<TrackRowObject> getTrackRow(TrackId id,
                                         std::span<rt::TrackPresentationField const> requestedFields) const;
```

### Constraint

This remains GTK/app-layer loading. Do not move row values into runtime projection.

### Acceptance Criteria

- Measurable improvement on large libraries.
- No runtime value loading API introduced.

## Full Verification Milestone

After phases 5, 6, 7, and 8, run:

```bash
./build.sh debug
```

If iteration speed requires narrower checks, run targeted build/test first, then full debug before considering the phase complete.
