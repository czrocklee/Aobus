# Linux GTK Allocation Optimization Plan

## Goal

Reduce avoidable allocations in the Linux GTK track presentation path without changing user-visible behavior.

The immediate focus is smooth scrolling and fast list/presentation switching. The highest impact area is `Gtk::ColumnView` row binding, because it runs repeatedly as GTK realizes and reuses list items. Playback and resize paths are secondary unless profiling proves otherwise.

## Scope

Primary files:

```text
app/linux-gtk/track/TrackColumnFactoryBuilder.cpp
app/linux-gtk/track/TrackRowObject.cpp
app/linux-gtk/track/TrackRowObject.h
app/linux-gtk/track/TrackFieldUi.cpp
app/linux-gtk/track/TrackFieldUi.h
app/linux-gtk/playback/TimeLabel.cpp
app/linux-gtk/playback/TimeLabel.h
app/linux-gtk/track/TrackColumnController.cpp
app/linux-gtk/track/TrackColumnController.h
```

Secondary files to inspect only if measurements justify it:

```text
app/linux-gtk/track/TrackColumnViewHost.cpp
app/linux-gtk/track/TrackRowCache.cpp
app/linux-gtk/track/TrackViewPage.cpp
app/linux-gtk/list/ListSidebarPanel.cpp
app/linux-gtk/list/QueryExpressionBox.cpp
```

## Baseline Findings

### High Frequency: Row Binding

`TrackColumnFactoryBuilder.cpp` currently performs avoidable work on every row bind:

- `setConnectionData()` stores each `sigc::connection` through `new sigc::connection`.
- Editable fields store three connections per bind: model, activate, and playing.
- Read-only fields store one connection per bind: playing.
- Editable fields call `row->getFieldText(field)` twice, once for the label and once for the entry.
- `Gtk::EventControllerKey` is created and added during bind for editable cells. Unbind removes connection qdata, but it does not remove that controller. Repeated bind/unbind cycles may accumulate controllers on reused entry widgets.

`TrackRowObject::getFieldText()` currently converts through the generic field UI reader:

```text
readRowText(...) -> std::string -> Glib::ustring
```

For Title, Artist, and Album, this is especially wasteful because those values already live as `Glib::ustring` properties.

`trackFieldUiDefinition(field)` performs a linear scan across the UI definition array. The current array has 25 entries. This is small in isolation, but it is called frequently by row binding and text lookup.

### Medium Frequency: Playback And Resize

`TimeLabel` updates on every GTK frame while playback is active. It formats and sets label text every tick even when the displayed second has not changed.

`TrackColumnController::updateTitlePositionVariable()` rebuilds a CSS string and calls `load_from_data()` when layout notifications fire. This can happen often during resize or column width changes.

### Low Frequency: Rebuild And Cache Fill

`TrackColumnViewHost::rebuild()` replaces the track table generation. This is a larger allocation event, but it is tied to presentation changes rather than scrolling.

`TrackRowCache::resolveDictionaryString()` constructs and caches `Glib::ustring` values on first dictionary access. After warmup, dictionary strings are reused.

## Priority Order

1. Fix per-bind object lifetime and allocations in `TrackColumnFactoryBuilder`.
2. Avoid duplicate field text reads during editable cell bind.
3. Replace linear `trackFieldUiDefinition()` lookup with constant-time lookup.
4. Avoid unnecessary `std::string` round trips for property-backed fields.
5. Add dirty checks to `TimeLabel`.
6. Deduplicate title-position CSS updates.
7. Inspect low-frequency paths only after the high-frequency changes are measured.

## Implementation Plan

### Phase 1: Establish A Measurable Baseline

Add temporary local instrumentation first, then use allocator tracing only when the counts need to be tied to heap allocation sites.

Suggested lightweight counters:

- Count `signal_bind()` and `signal_unbind()` calls in `TrackColumnFactoryBuilder`.
- Count `TrackRowObject::getFieldText()` calls by field.
- Count `trackFieldUiDefinition()` calls by field.
- Count editable entry key controllers after repeated bind/unbind cycles, if GTK exposes enough widget controller state for a debug assertion.
- Count `TimeLabel::updateLabel()` calls and actual label text changes.
- Count `TrackColumnController::updateTitlePositionVariable()` calls and actual CSS reloads.

If heap attribution is still needed, use an external allocator/profiling tool such as `heaptrack`, `valgrind --tool=massif`, or `perf` malloc probes where available. Keep this as a second step so the first pass remains quick and easy to repeat.

Measure these scenarios:

- Open a large list and scroll continuously.
- Switch between track presentations.
- Play a track for at least 10 seconds.
- Resize the window continuously for several seconds.

Record before/after numbers for:

- allocations per row bind;
- number of controllers attached to editable entry widgets after repeated bind cycles;
- `getFieldText()` calls during bind;
- `trackFieldUiDefinition()` calls during bind;
- `TimeLabel::updateLabel()` calls versus visible text changes;
- `TrackColumnController::updateTitlePositionVariable()` calls versus CSS reloads.

Acceptance criteria:

- The baseline can be reproduced locally.
- Temporary instrumentation is not committed unless it is useful as a permanent debug aid.

### Phase 2: Fix Row Bind Lifetime

Move editable-cell key controller creation out of bind. The intended location is the editable-cell setup path: create the `Gtk::Entry`, attach the `Gtk::EventControllerKey` immediately, then install the entry into the stack. Do not wait until bind, because bind runs repeatedly for reused widgets.

Replace per-bind heap allocation for `sigc::connection` storage. Preferred approaches, in order:

1. Store all bind-time connections in one small `BindData` object, for example `modelConnection`, `activateConnection`, and `playingConnection`, and attach that object to the list item or child widget.
2. Create the `BindData` object during setup and reuse it across bind/unbind for the cell lifetime.
3. Store connection members in a managed helper object attached to the list item for the widget lifetime.
4. If qdata must remain, allocate one `BindData` object instead of three independent `sigc::connection` objects, and reuse that storage instead of allocating on each bind.

Unbind must disconnect:

- model property connection;
- entry activate connection;
- playing property connection.

It must also leave reused widgets in a clean state:

- stack visible child reset to display mode;
- no duplicate key controllers;
- no stale row references kept alive by connected lambdas.

Acceptance criteria:

- Repeated bind/unbind does not increase key controllers on editable entries.
- No per-bind `new sigc::connection` remains, and bind uses at most existing reusable connection storage.
- Editable cells still commit on Enter and cancel on Escape.
- Playing row styling still updates when the playing track changes.

### Phase 3: Read Cell Text Once Per Bind

Structure bind so each path reads display text once. The static path should keep one field text read. The editable path should compute one field text value and use it for both the label and the entry:

```text
auto const text = row->getFieldText(field);
label->set_text(text);
entry->set_text(text);
```

Prefer a shared helper or local variable in `onTextColumnBind()` so static labels, editable labels, and editable entries cannot accidentally diverge.

Acceptance criteria:

- Static fields call `getFieldText()` once per bind.
- Editable fields call `getFieldText()` once per bind.
- Label and entry show identical text after bind.
- Existing inline edit behavior is unchanged.

### Phase 4: Make Field Definition Lookup Constant Time

Replace the linear scan in `trackFieldUiDefinition(field)` with a constant-time lookup.

Preferred approach:

- Build a static array indexed by `rt::TrackField` enum value.
- Store pointers into the existing `TrackFieldUiDefinition` array.
- Validate during construction or tests that every presentable runtime field has a matching UI definition.

This approach is only appropriate if `rt::TrackField` values are dense and the maximum enum value is small. If the enum is sparse, has large gaps, or may become sparse, use a generated `switch` lookup or a constexpr lookup table over the 25 known definitions instead. The implementation must not depend on declaration order unless tests enforce that order.

Acceptance criteria:

- `trackFieldUiDefinition(field)` no longer loops through all definitions.
- Existing tests for presentable fields still pass.
- Add or update tests for known fields and invalid/non-presentable lookup behavior.

### Phase 5: Avoid Property-backed String Round Trips

Optimize `TrackRowObject::getFieldText()` for fields that already have `Glib::ustring` storage:

- Title
- Artist
- Album
- Tags, if the generic field UI contract allows it cleanly

For these fields, return the existing `Glib::ustring` value without converting to `std::string` and back.

Important detail: `getTitle()`, `getArtist()`, and `getAlbum()` currently return `Glib::ustring` by value. A hot-path optimization must not pretend those are references. Either add safe const-reference accessors or dedicated field-text accessors backed by stable storage, or limit this phase to removing the `std::string` round trip while accepting a remaining `Glib::ustring` copy. Keep `Glib::Property` semantics correct and avoid returning references to temporaries.

Acceptance criteria:

- Title, Artist, and Album bind paths no longer use `readRowText() -> std::string -> Glib::ustring`.
- Metadata editing still updates label and entry after optimistic writes and rollback.
- Tests cover at least Title, Artist, Album, and one non-property-backed field.

### Phase 6: Add TimeLabel Dirty Checking

Track the last displayed values in `TimeLabel`.

For elapsed and default modes:

- only update when `posMs / 1000` changes or duration seconds change.

For duration mode:

- only update when duration seconds changes.

Reset must clear the cached display state.

Acceptance criteria:

- While playing, frame ticks continue, but `set_text()` happens only when visible text changes.
- Seek preview still updates immediately.
- Reset and new track start still display correct values.

### Phase 7: Deduplicate Title Position CSS Updates

Cache the last applied title position CSS value.

Avoid calling `std::format()` and `load_from_data()` when:

- title column is not found;
- view width and computed title position have not changed enough to alter the displayed CSS value;
- the generated CSS string would be identical to the previous one.

Start with exact generated-string deduplication. Add idle debounce only if profiling still shows resize churn.

Acceptance criteria:

- Repeated layout notifications with the same computed title position do not reload CSS.
- Column resize and presentation switches still update the title-position variable correctly.

### Phase 8: Review Low Frequency Paths

Inspect these paths after high-frequency changes are complete:

- `TrackColumnViewHost::rebuild()`
- `TrackRowCache::resolveDictionaryString()`
- `TrackViewPage::commitMetadataChange()`
- `ListSidebarPanel::bindSidebarListItem()`
- `QueryExpressionBox::updateCompletion()`

Only optimize them when there is a measured cost or the change is very small and low risk.

Acceptance criteria:

- No broad refactor is introduced just to reduce rare allocations.
- Any low-frequency optimization has a clear measurement or a clear local simplification.

## Testing Plan

Required tests:

- Unit tests for `trackFieldUiDefinition()` lookup behavior.
- Unit or integration-style tests for editable bind/unbind lifecycle where feasible.
- Tests for `TrackRowObject::getFieldText()` property-backed fields and at least one generic formatted field.
- Tests for `TimeLabel` dirty update behavior if the class can be made testable without GTK frame-clock fragility.
- Tests or focused assertions for title CSS update deduplication if the logic is extracted into a testable helper.

Manual regression checklist:

- Scroll a large track list quickly.
- Switch between built-in and custom presentations repeatedly.
- Edit Title, Artist, and Album inline.
- Press Escape in an inline editor.
- Start playback, pause, seek, and stop.
- Resize the window while track columns are visible.

Validation commands:

```text
./build.sh debug
./script/run-clang-tidy.sh
```

If changes are limited to targeted files, prefer targeted clang-tidy on the touched C++ files.

## Non-goals

- Rewriting `Gtk::ColumnView` ownership.
- Replacing `Glib::ustring` throughout the GTK frontend.
- Optimizing dictionary string cache warmup before measuring it.
- Introducing a custom allocator.
- Changing user-facing behavior or visual design.

## Rollout Strategy

Implement phases independently and verify after each phase. The recommended first patch should only cover Phase 2 and Phase 3, because those directly affect the hottest row-bind path and have the clearest correctness boundary.

Follow with Phase 4 and Phase 5 as a second patch. Phase 6 and Phase 7 can be separate smaller patches.
