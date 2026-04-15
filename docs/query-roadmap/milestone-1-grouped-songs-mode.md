# Milestone 1: Grouped Songs-Mode Foundation

## Objective

Deliver a production-quality grouped songs browser in the current right-hand table without yet changing saved-list semantics.

This milestone hardens the grouped songs implementation that is already partially present in the codebase.

The goal is not to invent a second browsing model. The goal is to make the existing songs-first pipeline reliable enough to act as the primary right-pane experience for `All Tracks` and inherited smart lists alike.

## Scope

In scope:

- enrich track row data with fields needed for grouping and sorting
- introduce an app-level presentation spec for the right pane
- render grouped sections in `Gtk::ColumnView` using native section headers
- make playback use visible sorted order rather than source insertion order
- verify that grouping works correctly on top of inherited smart-list membership, not only on `All Tracks`

Out of scope:

- persisted `group by` or `sort by` list syntax
- left navigation tree changes
- dedicated albums and artists presenters
- core query grammar changes
- changing list inheritance semantics

## Deliverables

1. A presentation-spec type in `app/` for songs-mode grouping and sorting.
2. Track-row accessors for the fields required by the initial grouping presets.
3. A `Gtk::SortListModel`-based right-pane pipeline with section headers.
4. Playback and selection logic that follows the visible sorted order.
5. At least one regression test or focused verification harness for sorted or grouped playback order if no existing test covers it.

## Initial Feature Set

Recommended first grouping presets:

- `None`
- `Artist`
- `Album`
- `Album Artist`
- `Genre`
- `Year`

Recommended initial sort presets:

- for `Artist`: `artist, album, discNumber, trackNumber, title`
- for `Album`: `albumArtist, album, discNumber, trackNumber, title`
- for `Album Artist`: `albumArtist, album, discNumber, trackNumber, title`
- for `Genre`: `genre, artist, album, discNumber, trackNumber, title`
- for `Year`: `year, artist, album, discNumber, trackNumber, title`

## Implementation Plan

### 1. Extend Row Data

Update the row-data provider so grouping does not need repeated ad-hoc LMDB reads during GTK sort callbacks.

Target additions in the provider-side DTO:

- album artist
- genre
- year
- disc number
- track number

Implementation notes:

- load the minimum storage tier required for the chosen first feature set
- keep the existing "owned DTO only" rule in [`TrackRowDataProvider`](file:///home/rocklee/dev/RockStudio/app/model/TrackRowDataProvider.h)
- if a field is unavailable, normalize to predictable empty-or-zero values so sort and group behavior stays stable

### 2. Extend `TrackRow`

Expose the additional grouping and sorting fields from [`TrackRow`](file:///home/rocklee/dev/RockStudio/app/TrackRow.h).

The row object should become the single source for GTK sorters and table factories.

### 3. Introduce Presentation Spec

Add an app-level type that describes songs-mode grouping and sorting independently of the list-query language.

Suggested shape:

```cpp
enum class GroupBy { None, Artist, Album, AlbumArtist, Genre, Year };

struct PresentationSpec {
  GroupBy groupBy = GroupBy::None;
  std::vector<SortTerm> sortBy;
};
```

This spec should start as page-local browsing state. Persist it later only if it proves worth keeping.

### 4. Consolidate The Existing Model Wiring

The current page already builds around:

```text
adapter model -> SortListModel -> MultiSelection -> ColumnView
```

Use this milestone to validate and close gaps in the existing pipeline:

- main sorter for visible row order
- section sorter for group boundaries
- `ColumnView::set_header_factory()` for native full-width section headers

Avoid synthetic fake rows for the normal grouped songs case.

### 5. Implement Native Section Headers Cleanly

Use `Gtk::ColumnView` section headers, backed by a section-capable model.

Header widget content should start simple:

- primary label: group title
- secondary text: track count, optional duration later

Avoid collapse or expand in this milestone. First make grouped browsing stable, then add collapsible headers later.

### 6. Rebind Playback To Visible Order

Review selection and activation paths in [`TrackViewPage`](file:///home/rocklee/dev/RockStudio/app/TrackViewPage.cpp#L112-L303).

Requirements:

- selected rows map to the sorted visible row positions
- activation emits the selected `TrackId` from the sorted model
- queueing and export paths use the same visible ordering contract
- this must hold for `All Tracks`, manual lists, and inherited smart lists loaded from parent membership

### 7. Minimal UI Entry Point

Keep a compact control surface for grouping or sorting.

Examples:

- a compact grouping dropdown near the filter entry
- a page-local developer toggle if the final UX is deferred

The important outcome is that the model and renderer are exercisable without needing milestone 2 first.

Because much of this milestone is already present in the current code snapshot, the implementation focus should be on gap-closing, correctness, and verification rather than broad redesign.

## Code Areas Expected To Change

- `app/TrackViewPage.*`
- `app/TrackListAdapter.*`
- `app/TrackRow.*`
- `app/model/TrackRowDataProvider.*`
- optionally `app/MainWindow.*` for wiring and verification seams

## Risks And Decisions

### Risk: GTK API Availability

Section headers on `Gtk::ColumnView` require GTK or gtkmm APIs introduced in 4.12.

Decision:

- prefer native section headers when available
- if the exact local gtkmm bindings block this, fall back temporarily to synthetic header rows only for the milestone branch, but keep the milestone design centered on native sections

### Risk: Sort Keys Need More Data Than Current Hot Path

Current row loading is optimized for title, artist, and album only.

Decision:

- accept broader row DTO loading in this milestone if needed
- refine storage-tier access later during the performance pass

## Verification Steps

### Build

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build /tmp/build --parallel"
```

### Automated Checks

- add or update focused tests for sort-key extraction and visible-order playback mapping if there is an existing suitable test layer
- run:

```bash
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

### Manual Checks

1. Open `All Tracks` and verify `Group: None` preserves current table behavior.
2. Switch to `Group: Artist` and verify rows are sorted into contiguous artist sections.
3. Activate a track in the middle of a grouped section and verify playback starts from that exact row.
4. Select multiple tracks across section boundaries and verify selected `TrackId`s reflect visible order.
5. Combine quick text filtering with grouping and verify section headers update correctly.
6. Test empty values for artist, album, genre, and year and verify they fall into predictable groups.
7. Open a smart list whose parent is also filtered and verify grouping plus playback operate over the inherited membership rather than the full library.

## Exit Criteria

- grouped songs mode is stable and playable
- visible ordering is deterministic
- grouping works without fake merged-column rows
- the current songs-first page is reliable enough to serve as the right pane for inherited smart lists
