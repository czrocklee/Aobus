# Track Field Registry One-Shot Implementation Plan

## Purpose

This plan implements the field-centric track model in one final state. It intentionally avoids landing a halfway design where `TrackPresentationField`, `TrackColumn`, and a new `TrackField` coexist as separate semantic dispatch systems.

Implementation may be done as multiple local commits, but the final merged state must satisfy all acceptance criteria in this document.

## Definition of Done

The migration is complete only when all of the following are true:

1. `rt::TrackField` is the only persisted/presentation field enum.
2. `rt::TrackPresentationField` has been removed.
3. GTK column state uses `rt::TrackField`, not `TrackColumn`.
4. `TrackColumn` has been removed or reduced to a non-semantic private implementation detail. Prefer removal.
5. Track field id conversion is centralized in runtime field registry functions.
6. GTK display/edit/drag behavior is centralized in GTK field UI registry functions.
7. `TrackViewPage` commits inline edits through field capabilities.
8. `TrackPropertiesDialog` dynamically builds editable and read-only rows from field definitions.
9. Existing presentation presets, custom view storage, and layout generation use `TrackField`.
10. Tests cover the registry, presentation specs, GTK column generation, inline edit patch writing, and Properties dialog field aggregation.

## Implementation Strategy

This is a one-shot migration with internal work slices. Do not stop after a slice unless the branch is private and known broken.

Recommended order:

1. Add runtime `TrackField` registry.
2. Convert runtime presentation specs and tests from `TrackPresentationField` to `TrackField`.
3. Add GTK `TrackFieldUiDefinition` registry with separate table-row readers and TrackView raw-value readers.
4. Convert GTK columns/factory/controller from `TrackColumn`/`TrackColumnLayout` to `TrackField`.
5. Convert `TrackRowObject` access from column text to field text.
6. Convert inline edit and drag-to-query to field capabilities.
7. Convert `TrackPropertiesDialog` to dynamic field-driven UI.
8. Remove obsolete enums, mappings, and duplicated dispatch.
9. Update tests and design docs.
10. Run targeted verification, then full debug build if feasible.

## Slice 1: Runtime Field Registry -- COMPLETED

### Files

Add:

```text
app/runtime/TrackField.h
app/runtime/TrackField.cpp
```

Update:

```text
app/runtime/CMakeLists.txt
app/runtime/StateTypes.h
app/runtime/TrackPresentationPreset.h
app/runtime/TrackPresentationPreset.cpp
app/runtime/TrackPresentationStore-related code if present
app/runtime/ProjectionTypes.h
app/runtime/ViewService.cpp
app/runtime/TrackListProjection.cpp
```

### Changes

1. Introduce `rt::TrackField`, `TrackFieldCategory`, `TrackFieldValueKind`, and `TrackFieldDefinition`.
2. Move field id parsing/formatting to `TrackField.cpp`.
3. Replace `TrackPresentationSpec::visibleFields` and `redundantFields` with `std::vector<TrackField>`.
4. Replace all `TrackPresentationField` references in runtime state, projection snapshots, runtime code, config conversion, and tests.
5. Delete `trackPresentationFieldId()` and `trackPresentationFieldFromId()` or replace call sites with `trackFieldId()` and `trackFieldFromId()`.
6. Preserve existing `TrackSortField` and `TrackGroupKey` for projection internals, but make their relationship to fields explicit in the registry.

### Registry Coverage

The first implementation must register all existing fields used anywhere in the GTK track table or properties dialog:

- metadata: title, artist, album, album artist, genre, composer, work
- numeric metadata: year, disc number, total discs, track number, total tracks
- tags: tags
- technical: duration, file path, codec, sample rate, channels, bit depth, bitrate, file size, modified time
- synthetic: display track number, technical summary, quality

Synthetic fields can be registered before they are exposed by built-in presets.

`TotalDiscs` and `TotalTracks` should be editable and may be presentable for custom columns, but they should not be added to existing built-in visible field lists unless that presentation explicitly needs them.

## Slice 2: Runtime Presets and Custom View State -- COMPLETED

### Files

Update:

```text
app/runtime/TrackPresentationPreset.cpp
app/runtime/TrackPresentationPreset.h
app/runtime/StateTypes.h
app/linux-gtk/track/TrackPresentationStore.cpp
app/linux-gtk/track/TrackCustomViewDialog.cpp
```

### Changes

1. Built-in presets use `rt::TrackField` values.
2. Custom view serialization writes field ids through `rt::trackFieldId()`.
3. Custom view deserialization reads field ids through `rt::trackFieldFromId()`.
4. Any dropdown/list for visible fields uses `trackFieldDefinitions()` filtered by `presentable`.
5. Any dropdown/list for sort fields can continue using `TrackSortField`, but labels should come from the matching `TrackFieldDefinition` where possible.
6. Group-by options in `TrackCustomViewDialog` are generated from `trackFieldDefinitions()` filtered by `groupable`, then converted through the field definition's `groupKey`.

### Acceptance Criteria

- There is no runtime reference to `TrackPresentationField`.
- Built-in presets still produce the same visible fields as before, except represented as `TrackField`.
- Unknown field ids in config are ignored safely.

## Slice 3: GTK Field UI Registry -- COMPLETED

### Files

Add:

```text
app/linux-gtk/track/TrackFieldUi.h
app/linux-gtk/track/TrackFieldUi.cpp
```

Update:

```text
app/linux-gtk/CMakeLists.txt
app/linux-gtk/track/TrackRowObject.h
app/linux-gtk/track/TrackRowObject.cpp
app/linux-gtk/track/TrackRowCache.h
app/linux-gtk/track/TrackRowCache.cpp
```

### Changes

1. Add GTK field UI definitions keyed by `rt::TrackField`.
2. Use separate readers for table cells and Properties dialog aggregation:
   - table cells read display text from `TrackRowObject` plus `TrackRowCache`;
   - Properties dialog reads raw comparable values from `TrackView` plus `DictionaryStore`, then formats only for UI.
3. Centralize display formatting for:
   - title
   - dictionary-backed metadata fields
   - numeric metadata fields
   - duration
   - tags
   - file path
   - codec
   - sample rate
   - channels
   - bit depth
   - bitrate
   - file size
   - modified time
   - synthetic display track number
   - synthetic technical summary
4. Centralize patch writing for editable fields:
   - title
   - artist
   - album
   - album artist
   - genre
   - composer
   - work
   - year
   - disc number
   - total discs
   - track number
   - total tracks
5. Keep tag changes routed through tag editing services, but register `TrackField::Tags` as a presentable/read-only field for table and properties display.
6. Move preformatted table strings out of `TrackRowObject::populate()` where possible. `TrackRowObject` stores raw values; field UI readers/formatters produce strings on demand.

### Acceptance Criteria

- All field-specific display formatting lives in `TrackFieldUi.cpp` or helpers used only by that registry.
- `TrackRowObject::getColumnText(...)` no longer exists.
- Field display reads work from a `TrackRowObject` for table cells, while Properties dialog aggregation compares raw values read from `TrackView`.

## Slice 4: Column Model Becomes Field-Based -- COMPLETED

### Files

Update:

```text
app/linux-gtk/track/TrackPresentation.h
app/linux-gtk/track/TrackPresentation.cpp
app/linux-gtk/track/TrackColumnController.h
app/linux-gtk/track/TrackColumnController.cpp
app/linux-gtk/track/TrackColumnFactoryBuilder.h
app/linux-gtk/track/TrackColumnFactoryBuilder.cpp
app/linux-gtk/track/ColumnVisibilityModel.h
app/linux-gtk/track/ColumnVisibilityModel.cpp
```

### Changes

1. Remove `TrackColumn` enum.
2. Remove `TrackColumnLayout` as a semantic layout model.
3. Let `TrackPresentationSpec::visibleFields` define column order and visibility.
4. Keep only a narrow field-keyed column view state for UI concerns that are not presentation semantics, such as manually resized widths.
5. Generate default columns from `trackFieldUiDefinitions()` filtered by the runtime definition's `presentable` flag.
6. Use `rt::trackFieldId(field)` for `Gtk::ColumnViewColumn::set_id()`.
7. Use `rt::trackFieldFromId(column->get_id())` when capturing width state.
8. Delete `trackColumnForPresentationField()` and `redundantFieldToColumn()`.
9. Delete `ColumnVisibilityModel`; `TrackColumnController` directly calls `Gtk::ColumnViewColumn::set_visible()` for columns whose fields are in the active presentation.
10. Replace hard-coded title-column checks in `updateTitlePositionVariable()` with field-id checks such as `col->get_id() == rt::trackFieldId(rt::TrackField::Title)`.

### Acceptance Criteria

- `TrackColumnController` stores bindings keyed by `rt::TrackField`.
- No switch maps presentation fields to columns.
- Column order, width, expansion, and visibility still behave as before for built-in presets.
- There is no `ColumnVisibilityModel` switch or property-per-field replacement.

## Slice 5: Cell Factories, Drag, and Playing Style -- COMPLETED

### Files

Update:

```text
app/linux-gtk/track/TrackColumnFactoryBuilder.h
app/linux-gtk/track/TrackColumnFactoryBuilder.cpp
app/linux-gtk/track/TrackViewPage.h
app/linux-gtk/track/TrackViewPage.cpp
```

### Changes

1. `buildColumnFactory()` accepts a field-based column definition or `rt::TrackField`.
2. Cell text is read through `TrackFieldUiDefinition::readRowText`.
3. Drag content uses `TrackFieldUiDefinition::dragQueryPrefix`.
4. Editable cell setup uses `TrackFieldUiDefinition::inlineEditable`.
5. Inline edit commit writes `MetadataPatch` through `TrackFieldUiDefinition::writePatch`.
6. Optimistic row update uses field-based row setters and emits a field-keyed row change signal. Fields without safe optimistic update support use row-cache invalidation and reload instead.
7. Existing `Title`/`Artist`/`Album` `Glib::Property` notifications are replaced or bridged through the same field-keyed row change signal so the factory has one reactive path.
8. Playing-title styling uses an explicit field capability or checks `field == rt::TrackField::Title` in a local style predicate. Prefer a capability if it avoids another one-off branch.

### Acceptance Criteria

- No Artist/Album/Genre branch exists for drag query prefixes.
- No Title/Artist/Album branch exists for inline edit patch generation.
- Inline edit behavior remains correct for title, artist, and album.
- If additional editable fields are visible as columns, they use the same field-driven path.
- Numeric editable fields do not require adding one new `Glib::Property` member per field to `TrackRowObject`.

## Slice 6: Properties Dialog Becomes Field-Driven -- COMPLETED

### Files

Update heavily:

```text
app/linux-gtk/tag/TrackPropertiesDialog.h
app/linux-gtk/tag/TrackPropertiesDialog.cpp
```

### Changes

1. Remove `AggregatedFields` with one member per field.
2. Remove fixed widget members such as `_titleEntry`, `_artistEntry`, `_yearSpin`, etc.
3. Add generic editor/aggregate state keyed by `rt::TrackField`.
4. Extract pure helper logic for field aggregation and patch building before wiring GTK widgets, so mixed-value behavior can be unit-tested without instantiating the dialog.
5. Build the Metadata tab from fields where `propertyDialogEditable == true`.
6. Build the Properties tab from fields where `propertyDialogReadonly == true`.
7. Centralize widget creation in a helper such as `createFieldWidget(field, valueKind)`:
   - text-like editable fields use `Gtk::Entry`;
   - numeric editable fields use `Gtk::SpinButton` with field-specific ranges;
   - read-only fields use `Gtk::Label`.
8. Use raw field values, not formatted display strings, for aggregation.
9. Use generic mixed-value logic:
   - first raw value initializes aggregate;
   - different subsequent raw value marks mixed.
10. Save by iterating editable editors and calling `writePatch` for non-mixed changed raw values.
11. Keep row cache invalidation after a successful mutation.
12. Keep tag editing as a separate editor flow, but make tag display use `TrackField::Tags`.

### Acceptance Criteria

- Adding a new editable metadata field to the registry adds it to Properties dialog without editing dialog-specific switch/if chains.
- Adding a new read-only technical field to the registry adds it to the Properties tab without editing dialog-specific switch/if chains.
- Multi-track mixed values still show as mixed and are not overwritten unless explicitly editable in the final UX.
- Existing Save behavior for non-mixed fields is preserved.
- Aggregation and patch-building helpers have unit coverage without depending on GTK dialog construction.

## Slice 7: Remove Obsolete Dispatch and Types -- COMPLETED

### Required Removals

Remove or rewrite all occurrences of:

```text
TrackPresentationField
TrackColumn::
TrackColumnDefinition
trackColumnForPresentationField
redundantFieldToColumn
getColumnText(TrackColumn)
ColumnVisibilityModel
TrackColumnLayout
```

Search commands:

```bash
rg -n "TrackPresentationField|TrackColumn::|TrackColumnDefinition|trackColumnForPresentationField|redundantFieldToColumn|getColumnText|ColumnVisibilityModel|TrackColumnLayout" app test include lib
```

Any remaining match must be either a changelog/design document reference or a temporary test helper being actively removed.

## Slice 8: Tests -- COMPLETED

### Runtime Tests

Update or add:

```text
test/unit/runtime/TrackFieldTest.cpp
test/unit/runtime/TrackPresentationPresetTest.cpp
test/unit/runtime/ViewServiceTest.cpp
test/unit/runtime/TrackListProjectionTest.cpp
```

Coverage:

1. `trackFieldDefinitions()` contains expected ids and no duplicate ids.
2. `trackFieldFromId(trackFieldId(field)) == field` for every field.
3. Presentation fields in built-in presets are presentable.
4. Sort terms in built-in presets map to sortable fields.
5. Group keys in built-in presets map to groupable fields.
6. Normalization removes duplicate visible/redundant fields.
7. Custom view serialization/deserialization uses track field ids.

### GTK Track Tests

Update or add:

```text
test/unit/linux-gtk/track/TrackPresentationTest.cpp
test/unit/linux-gtk/track/TrackRowCacheTest.cpp
test/unit/linux-gtk/track/TrackListAdapterTest.cpp
```

Coverage:

1. Default column layout is generated from presentable field definitions.
2. Built-in presentation layouts have the expected field order.
3. Expanding columns are selected from field UI definitions.
4. `trackFieldUiDefinition()` exists for every presentable field.
5. Display text readers return expected values for dictionary, numeric, duration, tag, technical, and synthetic fields.
6. Editable field writers produce expected `MetadataPatch` values.
7. Drag query prefixes are present for artist, album, and genre.
8. Title-position CSS logic finds the title column by field id, not by hard-coded display title.

### Properties Dialog Tests

Add if the existing test harness can instantiate the dialog headlessly:

```text
test/unit/linux-gtk/tag/TrackPropertiesDialogTest.cpp
```

Coverage:

1. Editable rows are generated from editable field definitions.
2. Read-only rows are generated from read-only field definitions.
3. Multi-track mixed aggregation compares raw values and marks differing values as mixed.
4. Save writes only non-mixed changed fields.
5. Save invalidates all selected row-cache entries after successful mutation.

If GTK dialog instantiation is hard in the current harness, extract the field aggregation and patch-building logic into small testable helpers in the same production file or a private helper header.

## Slice 9: Documentation Updates -- COMPLETED

Update:

```text
doc/design/track-presentation-presets.md
doc/design/track-presentation/01-architecture.md
doc/design/track-presentation/02-runtime-design.md
doc/design/track-presentation/03-gtk-integration.md
doc/design/track-presentation/06-testing-plan.md
```

Required doc changes:

1. Replace `TrackPresentationField` language with `TrackField`.
2. State that columns are field presentation surfaces.
3. State that Properties dialog consumes the same field registry.
4. Document synthetic fields.
5. Document the no-runtime-display-values boundary.

## Verification

Run from project root.

Targeted runtime tests:

```bash
nix-shell --run "cmake --build /tmp/build/debug --target ao_test --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test [runtime]"
```

Targeted GTK track/tag tests:

```bash
nix-shell --run "cmake --build /tmp/build/debug --target ao_test --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test [linux-gtk][track] [linux-gtk][tag]"
```

Clang-tidy for changed C++ files:

```bash
./script/run-clang-tidy.sh
```

Full debug validation before final handoff:

```bash
./build.sh debug
```

## Manual Checks

1. Launch GTK app.
2. Open All Tracks.
3. Switch through every built-in presentation.
4. Verify columns match each presentation.
5. Verify group headers still appear for grouped presentations.
6. Inline edit title, artist, and album.
7. If numeric inline editing is enabled, edit year/track/disc fields.
8. Drag artist, album, and genre cells into a query/list creation target if supported.
9. Open Properties for one track.
10. Verify editable metadata rows appear from the registry.
11. Verify technical read-only rows appear from the registry.
12. Open Properties for multiple tracks with differing values.
13. Verify mixed fields are marked and not overwritten accidentally.
14. Save a metadata change and confirm the table refreshes.
15. Edit tags through the existing tag editor and confirm the Tags field display updates.

## Review Checklist

- [ ] No runtime dependency on GTK/Glib.
- [ ] No duplicate semantic field enum remains.
- [ ] No `TrackColumn::Artist`-style feature dispatch remains.
- [ ] No Properties dialog field-specific if-chain remains.
- [ ] Properties dialog aggregation compares raw values rather than formatted strings.
- [ ] Column visibility is applied directly by field-keyed controller bindings; no expanded `ColumnVisibilityModel` remains.
- [ ] Title column styling and position calculations use field ids, not display titles.
- [ ] Registry entries cover all existing metadata, technical, tag, and synthetic fields.
- [ ] Every editable field has a patch writer.
- [ ] Every presentable field has a display reader.
- [ ] Every custom-view persisted field id round-trips through `TrackField` helpers.
- [ ] Tests cover registry completeness and duplicate-id prevention.
- [ ] Design docs describe field-centric presentation rather than column-centric presentation.

All checklist items are now satisfied and verified.

## Key Differences Between Plan and Actual Implementation

During implementation, the following decisions deviated from the original plan:

| Area | Plan | Actual | Reason |
| --- | --- | --- | --- |
| `TrackColumnViewState` storage | `std::unordered_map` (explored as alternative) | `std::array<std::int32_t, kTrackFieldCount>` (confirmed) | Simpler, compile-time-sized, no heap allocations |
| Visibility model | Bitset array (explored) | Direct `Gtk::ColumnViewColumn::set_visible()` | Simpler; avoids another indirection layer |
| `TrackRowObject` values | Preformatted strings in `populate()` | Raw values stored; UI readers/formatters produce strings on demand | Cleaner separation; raw values needed for Properties dialog mixed-value detection |
| Properties dialog `Tags` field | Expected to appear in metadata tab | Explicitly skipped (`TrackField::Tags` handled by `TagEditController`) | Tags have their own editor surface; displaying them as editable in metadata tab would conflict |
| `TrackFieldRawValue` variants | 5 alternatives (`std::monostate`, `std::string`, `std::uint16_t`, `std::uint32_t`, `Duration`) | 6 alternatives (adds `std::uint64_t` for `FileSize`/`ModifiedTime`) | `FileSize` and `ModifiedTime` require 64-bit range |
| UI registry array | `constexpr` | `static auto const` (non-constexpr) | GCC cannot handle lambda-to-function-pointer conversion in `constexpr` |
| UI registry entries | Lambdas or named functions | Function pointers via `+` prefix on captureless lambdas | Named functions would add boilerplate; `+` prefix converts lambda to function pointer |
| Runtime registry array | `constexpr std::to_array` | `constexpr std::to_array` with `static_assert` for 25 entries | As planned; compile-time enforcement added |

### Test Results

| Test suite | Assertions | Cases |
| --- | --- | --- |
| `ao_test` (runtime + GTK) | 12,407 | 426 |
| `ao_test_gtk` | 703 | 29 |
