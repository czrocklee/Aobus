---
id: presentation.track-list
type: spec
status: current
domain: presentation
summary: Defines track-list grouping, stable sorting, classical movement order, field suppression, and presentation recommendation.
---
# Track-list presentation

## Scope

This specification defines how a `TrackPresentationSpec` shapes projected rows without changing source membership.
Exact built-in preset identities belong to the [track preset reference](../../reference/presentation/track-preset.md).
Runtime field ids and their sort/group capabilities belong to the [track field catalog](../../reference/library/model/track-field.md).
Expression syntax and membership remain owned by the [track expression architecture](../../architecture/track-expression.md) and query contracts.

## Code boundary

This contract spans the **application runtime** and **UIModel** layers identified by the [system architecture](../../architecture/system-overview.md) and refined by the [presentation architecture](../../architecture/presentation.md).
Runtime presentation values, built-ins, normalization, and projection interpretation are public under `app/include/ao/rt/` and implemented under `app/runtime/`; recommendation and catalog adaptation are public under `app/include/ao/uimodel/` and implemented under `app/uimodel/` without depending on GTK or TUI types.

## Invariants

- Presentation affects grouping, ordering, visible fields, and redundant-field suppression, never list membership.
- A quick filter changes the source supplied to a presentation but does not select a new presentation.
- An empty sort preserves source order exactly.
- Sorts are stable for equal keys.
- A redundant field is suppressed only when the group header presents the same fact.
- Album groups use `(album artist, album)` identity; Work groups use `(composer, work)` identity.
- Movement sorting compares numeric movement number rather than movement-name text.

## Grouping and sorting

Group headers contain primary text and, for compound album/work keys, secondary album-artist or composer text.
Unknown group values produce the field's unknown label rather than merging with an unrelated concrete value.

Only `TrackSortField` values resident in projection snapshots are sortable.
Manifest-backed file size and modified time remain display-only.

When a grouped spec has an empty redundant-field set, normalization may borrow the first built-in preset's suppression set for that group key.
Custom shapes that require no suppression provide explicit visible-field intent.

## Classical behavior

Work grouping merges recordings of the same `(composer, work)` into one section.
The classical presets keep recordings contiguous with this core sort chain, prefixed by their grouping concern where applicable:

```text
Composer -> Work -> Year -> Album -> Movement -> DiscNumber -> TrackNumber -> Title
```

Album precedes Movement so different performances do not interleave movement-by-movement.
Movement name is the visible row label, while movement number controls order.

Classical Works groups by Work and suppresses composer/work fields already represented by its header.
Classical Composers groups by Composer and keeps Work visible because multiple works occur in the section.
Classical Conductors groups by Conductor and keeps Composer, Work, Ensemble, and Movement visible.

## Recommendation

Manual lists recommend the source-order preset so reorder controls correspond to visible order.
Smart-list recommendation inspects successfully parsed filter variables with this priority:

| Filter signal | Recommended intent |
|---|---|
| Work | Classical Works |
| Composer | Classical Composers |
| Sample rate, bit depth, or bitrate | Technical |
| Tag | Tagging |
| Genre or year | Albums |
| Album artist | Artists |
| Artist or album | Albums |
| No recognized signal | Albums |

Recommendation is a one-way read of expression structure.
It neither compiles the predicate nor changes Smart List or transient-filter membership.

## Failure and lifecycle

An invalid custom spec is normalized at the presentation boundary where possible; query parsing failure falls back to the default recommendation rather than changing source content.
Changing the active presentation triggers a projection rebuild as defined by [track-list projection](../library/projection/track-list.md).

GTK row adapters cache display strings on `TrackRowObject` so recycled cells do not reformat computed fields on each scroll bind.
Computed values are invalidated when one of their contributing row values changes.
Now-playing row style is driven by the current playing id in `TrackListModel`; visible cells receive a dedicated change observation and newly bound/recycled rows read the same model state.

## Implementation map

- [`TrackPresentation.h`](../../../app/include/ao/rt/TrackPresentation.h) defines the spec and preset values.
- [`TrackPresentation.cpp`](../../../app/runtime/TrackPresentation.cpp) owns built-ins and normalization.
- UIModel presentation code under [`app/uimodel/library/presentation/`](../../../app/uimodel/library/presentation/) owns catalog and recommendation adaptation.

## Test map

- [`TrackPresentationTest.cpp`](../../../test/unit/runtime/TrackPresentationTest.cpp) locks built-ins and normalization.
- Sorting and grouping tests under [`test/unit/runtime/projection/`](../../../test/unit/runtime/projection/) lock compound groups and classical order.
- Presentation recommendation tests under [`test/unit/uimodel/library/presentation/`](../../../test/unit/uimodel/library/presentation/) lock selection behavior.
- [`TrackRowCacheTest.cpp`](../../../test/unit/linux-gtk/track/TrackRowCacheTest.cpp) and [`TrackListModelTest.cpp`](../../../test/unit/linux-gtk/track/TrackListModelTest.cpp) protect GTK row caching and now-playing model state.

## Related documents

- [Track preset reference](../../reference/presentation/track-preset.md)
- [Track model](../../reference/library/model/track.md)
- [Track field catalog](../../reference/library/model/track-field.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Track expression architecture](../../architecture/track-expression.md)
- [Track filtering](track-filter.md)
- [List presentation preference](list-preference.md)
- [Track-column layout](track-column-layout.md)
