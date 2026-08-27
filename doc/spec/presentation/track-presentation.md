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
- A flat presentation with an empty sort preserves source order exactly.
- Sorts are stable for equal keys.
- Every group identity occupies one contiguous row range, including when a
  custom sort omits its grouping field.
- A redundant field is suppressed only when the group header presents the same fact.
- Every materialized group section has a nonempty primary display value after UIModel formatting.
- Album groups use `(album artist, album)` identity; Work groups use `(composer, work)` identity.
- Movement sorting compares numeric movement number rather than movement-name text.

## Grouping and sorting

Runtime group headers contain three structured slots.
Each slot retains absence, raw text, a numeric year, or a typed `MissingTrackValueKind`; compound album/work keys retain the secondary album-artist or composer value independently.
Every grouping populates the primary slot with raw text, a numeric year, or a typed missing value before materializing a section.
Unknown group values remain distinct semantic keys rather than merging with an unrelated concrete value or becoming English inside runtime.
UIModel resolves the slots through feature presentation functions over `MessageCatalog`, and frontend adapters own only markup and geometry.

Text display, group identity, and ordering use separate keys:

```text
display text   = admitted NFC text
group identity = NFC(Default_Case_Folding(unstripped text))
ordering input = NFC(Default_Case_Folding(article-stripped text))
interactive ordering key = ICU(startup locale, fixed attributes).sortKey(ordering input)
non-locale ordering key  = ordering input bytes
```

Group identity is locale-independent. It merges Unicode-caseless spellings
such as `MÉTAL`/`métal` and `Straße`/`STRASSE`, while the first row in final
projection order supplies the raw visible heading. Leading `the`, `a`, and
`an` are removed only from ordering input, so `The Doors` and `Doors` sort
together but remain separate groups.

GTK, TUI, and WinUI use their canonical startup locale for textual ordering.
The collator uses secondary strength, non-ignorable punctuation, case level
off, and numeric collation off. Secondary-only ties such as `ABC`/`ＡＢＣ` or
some width and kana variants remain distinct identities; the complete identity
stage places them deterministically and keeps each group contiguous. A runtime
without an interactive policy compares the same default-folded ordering input
as deterministic UTF-8 bytes; group identity uses the same locale-independent
fold over the unstripped value.

Grouped comparison orders the grouping components first, then the complete
unstripped identity, then the configured row sort terms. Configured grouping
terms retain their authored order and direction; missing compound components
are supplied in the direction of the first authored grouping term, or ascending
when none is authored. This keeps Album `(album artist, album)` and Work
`(composer, work)` identities contiguous even for custom presentation shapes.

Textual numeric characters remain lexical. Numeric track fields, including
movement number, retain their typed numeric comparators and never enter locale
collation.

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

Saved-List recommendation inspects successfully parsed local-filter variables with this priority:

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

Canonical variable names and their documented aliases resolve through the same typed core descriptor before UIModel applies this priority.
Recommendation is a one-way read of expression structure.
An empty or invalid expression and All Tracks fall back to Albums.
It neither compiles the predicate nor changes saved-List or transient-filter membership.

Manual Order is the built-in flat presentation whose `sortBy` is empty.
It preserves the effective source sequence produced by the saved List's rank overlay.
It is selected explicitly by the user or by the New Playlist template; storage does not classify a List as manual and recommendation does not infer Manual Order from an empty filter.
All Tracks may still preserve source order in a custom flat unsorted presentation, but it has no writable rank overlay and its Manual Order choice is disabled.

## Failure and lifecycle

An invalid custom spec is normalized at the presentation boundary where possible; query parsing failure falls back to the default recommendation rather than changing source content.
Changing the active presentation triggers a projection rebuild as defined by [track-list projection](../library/projection/track-list.md).

GTK row adapters cache display strings on `TrackRowObject` so recycled cells do not reformat computed fields on each scroll bind.
Computed values are invalidated when one of their contributing row values changes.
Now-playing row style is driven by the current playing id in `TrackListModel`; visible cells receive a dedicated change observation and newly bound/recycled rows read the same model state.

## Implementation map

- [`TrackPresentation.h`](../../../app/include/ao/rt/TrackPresentation.h) defines the spec and preset values.
- [`TrackPresentation.cpp`](../../../app/runtime/TrackPresentation.cpp) owns built-ins and normalization.
- [`TrackGroupHeadingPresentation.cpp`](../../../app/uimodel/library/presentation/TrackGroupHeadingPresentation.cpp) resolves runtime heading values through the shared catalog.
- UIModel presentation code under [`app/uimodel/library/presentation/`](../../../app/uimodel/library/presentation/) owns catalog and recommendation adaptation.

## Test map

- [`TrackPresentationTest.cpp`](../../../test/unit/runtime/TrackPresentationTest.cpp) locks built-ins and normalization.
- Sorting and grouping tests under [`test/unit/runtime/projection/`](../../../test/unit/runtime/projection/) lock compound groups and classical order.
- Presentation recommendation tests under [`test/unit/uimodel/library/presentation/`](../../../test/unit/uimodel/library/presentation/) lock selection behavior.
- [`TrackRowCacheTest.cpp`](../../../test/unit/linux-gtk/track/TrackRowCacheTest.cpp) and [`TrackListModelTest.cpp`](../../../test/unit/linux-gtk/track/TrackListModelTest.cpp) protect GTK row caching and now-playing model state.

## Related documents

- [Track preset reference](../../reference/presentation/track-preset.md)
- [Presentation text catalog](../../reference/presentation/text-catalog.md)
- [Track model](../../reference/library/model/track.md)
- [Track field catalog](../../reference/library/model/track-field.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Track expression architecture](../../architecture/track-expression.md)
- [Track filtering](track-filter.md)
- [List presentation preference](list-preference.md)
- [Track-column layout](track-column-layout.md)
