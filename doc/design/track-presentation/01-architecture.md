# Track Presentation Architecture

## Purpose

This document defines the architecture for replacing the current group-by/columns table controls with high-level track presentation presets.

## Product Direction

The main track table should answer user intent:

- “show this as albums”
- “show this by artists”
- “show this as classical works”
- “show this for tagging/curation”
- “show this through my custom view”

It should not make normal users compose low-level table state from:

- group-by field
- sort terms
- visible columns
- column order

Those low-level controls still exist conceptually, but they belong in a custom view editor.

## Current State

### GTK controls

`TrackViewPage` currently exposes:

- filter entry
- group-by dropdown
- columns menu button

The group dropdown calls `ViewService::setGrouping()`. The columns button is backed by `TrackColumnController` and `TrackColumnLayoutModel`.

### Runtime presentation

Runtime currently treats group-by as the source of truth:

```cpp
presentationForGroup(TrackGroupKey groupBy)
```

That helper maps a group key to:

- effective sort terms
- redundant sort/display fields

This is too primitive for the target design because presentation should be selected by scenario, not derived from group-by.

### Projection and row loading

`TrackListProjection` already accepts group and sort:

```cpp
setPresentation(TrackGroupKey groupBy, std::vector<TrackSortTerm> sortBy)
```

It publishes ordered `TrackId`s and projection deltas. GTK resolves row values lazily.

Current value-loading path:

```text
TrackListProjection
  -> ordered TrackId sequence / deltas
  -> TrackListAdapter / ProjectionTrackModel
  -> TrackRowCache::getTrackRow(trackId)
  -> LMDB TrackStore reader
  -> TrackRowObject
  -> TrackColumnFactoryBuilder reads TrackRowObject values
```

This path remains the default path. Runtime does not materialize display rows.

## Target Layer Responsibilities

```text
Runtime
  - presentation id
  - built-in preset registry
  - custom presentation spec once loaded by the app
  - groupBy
  - sortBy
  - semantic visible fields
  - semantic redundant fields
  - active presentation per runtime view
  - projection sorting/grouping

GTK
  - preset selector widget
  - custom view editor UI
  - TrackPresentationField -> TrackColumn mapping
  - TrackColumnLayout generation
  - Gtk::ColumnViewColumn instances
  - column widths
  - cell factories
  - inline editing widgets
  - lazy row value loading through TrackRowCache

Library / LMDB
  - persisted track metadata
  - dictionary strings
  - resource and property data
```

## Boundary Rule

Runtime owns the presentation **schema**, not presentation **data**.

Allowed runtime concepts:

```cpp
TrackPresentationField::Title
TrackPresentationField::Artist
TrackPresentationField::Duration
```

Disallowed runtime concepts:

```cpp
Gtk::ColumnViewColumn
Glib::ustring
TrackColumnLayout
TrackRowObject
std::string valueFor(TrackId, TrackPresentationField)
```

## Data-loading Boundary

Runtime presentation includes `visibleFields`, but this does not imply runtime loads field values.

Do not add APIs like:

```cpp
std::string ViewService::fieldValue(TrackId id, TrackPresentationField field);
std::string TrackListProjection::fieldValueAt(std::size_t row, TrackPresentationField field);
```

Do not change projection deltas into row materialization deltas:

```cpp
struct TrackListRowSnapshot
{
  TrackId id;
  std::vector<TrackFieldValue> values;
};
```

GTK may later optimize `TrackRowCache` based on requested fields, but that is an app/GTK-layer optimization and should be profiling-driven.

## Built-in Presets

Initial built-ins should be few and opinionated.

| ID | Label | Group by | Sort by | Visible fields | Redundant fields |
| --- | --- | --- | --- | --- | --- |
| `songs` | Songs | None | Artist, Album, Disc, Track, Title | Title, Artist, Album, Duration, Tags | none |
| `albums` | Albums | Album | AlbumArtist, Album, Disc, Track, Title | Track, Title, Duration, Year, Tags | Album, AlbumArtist |
| `artists` | Artists | Artist | Artist, Album, Disc, Track, Title | Album, Track, Title, Duration, Tags | Artist |
| `album-artists` | Album Artists | AlbumArtist | AlbumArtist, Album, Disc, Track, Title | Album, Track, Title, Artist, Duration, Year | AlbumArtist |
| `classical-composers` | Classical: Composers | Composer | Composer, Work, Album, Disc, Track, Title | Work, Title, Artist, Album, Duration, Year | Composer |
| `classical-works` | Classical: Works | Work | Composer, Work, Disc, Track, Title | Composer, Title, Artist, Album, Duration | Work |
| `genres` | Genres | Genre | Genre, Artist, Album, Disc, Track, Title | Artist, Album, Title, Duration, Tags | Genre |
| `years` | Years | Year | Year, Artist, Album, Disc, Track, Title | Artist, Album, Title, Duration, Tags | Year |
| `tagging` | Tagging | None | Artist, Album, Disc, Track, Title | Title, Artist, Album, Genre, Year, Tags | none |

## UX Shape

Main toolbar target:

```text
[ Quick filter or expression...                         ]  View: [ Albums ▼ ]
```

Preset menu target:

```text
View
  Songs
  Albums
  Artists
  Album Artists
  Classical: Composers
  Classical: Works
  Genres
  Years
  Tagging
  ─────────────
  Custom Views
    My Classical Library
    DJ Prep
  ─────────────
  Create Custom View...
  Manage Custom Views...
```

The direct group-by dropdown and columns menu should disappear from the main toolbar. The underlying capabilities move into custom view editing.

## Architectural Invariants

1. A presentation spec is the source of truth for group, sort, and visible semantic fields.
2. Group-by must not derive sort-by globally anymore.
3. `TrackListProjection` continues to sort and group by runtime spec, but does not own GTK columns.
4. GTK maps semantic fields to columns and keeps all rendering details.
5. Runtime must not depend on `app/linux-gtk`.
6. Built-in presets are read-only.
7. Custom views are user-owned and may be persisted separately from the music library.
