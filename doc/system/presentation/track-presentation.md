---
id: presentation.track-list
---

# Track-list presentation

## Scope

This specification defines how a `TrackPresentationSpec` shapes projected rows without changing source membership.
Exact built-in preset identities belong to the [track preset reference](../../reference/presentation/track-preset.md).
Runtime field ids and their sort/group capabilities belong to the [track field catalog](../../reference/library/model/track-field.md).
Expression syntax and membership remain owned by the [track expression architecture](../query/README.md) and query contracts.

## Code boundary

This contract spans the **application runtime** and **UIModel** layers identified by the [system architecture](../overview.md) and refined by the [presentation architecture](README.md).
Runtime presentation values, built-ins, normalization, and projection interpretation are public under `app/include/ao/rt/` and implemented under `app/runtime/`; recommendation and catalog adaptation are public under `app/include/ao/uimodel/` and implemented under `app/uimodel/` without depending on GTK or TUI types.

## Invariants

- Presentation affects grouping, ordering, visible fields, and redundant-field suppression, never list membership.
- A quick filter changes the source supplied to a presentation but does not select a new presentation.
- A flat presentation with an empty sort preserves source order exactly.
- Sorts are stable for equal keys relative to the current source order.
- Every group identity occupies one contiguous row range, including when a
  custom sort omits its grouping field.
- A redundant field is suppressed only when the group header presents the same fact.
- Every materialized group section has a nonempty primary display value after UIModel formatting.
- Album groups use `(album artist, album)` identity; Work groups use `(composer, work)` identity.
- An Album group whose tracks have no album artist keeps that empty identity, but its heading names the artist that more than half of its tracks with an artist name, by group identity and with that artist's first row's text, or Various Artists when no artist has such a majority. A guest artist on one track of an album therefore leaves the album's own artist in the heading, while two artists with equal track counts make Various Artists. Tracks without an artist do not count, and with none the heading shows the missing-artist value. Two albums with the same title and no album artist share that empty identity and form one group, so the vote covers the tracks of both: 10 Queen tracks and 3 ABBA tracks under one title show Queen, which hides the merge.
- Movement sorting compares numeric movement number rather than movement-name text.

## Table rows and display coordinates

The [shared row formatter](../../../app/include/ao/uimodel/field/TrackRowField.h) supplies field text without filling in placeholders or substituting a title. The adapter supplies those choices without changing metadata or sort keys. AppKit shows the stored title, even when empty. WinUI and TUI use the stored title when nonempty, otherwise the filename if a path exists—even if its filename is empty—and a localized track-id label only if there is no path. TUI uses native path spelling, WinUI uses generic path spelling, and only a placeholder for an empty cell gets the terminal's dim dash style; a literal `-` is ordinary text.

The AppKit and WinUI tables build a shared display index from the projection. A group heading has a display position and points to the start of its section, but it is not a selectable or playable track. Group ranges must be nonempty, contiguous, and cover all projection rows; no groups gives a flat index. A rejected reset leaves the index unchanged. AppKit treats that rejection as an invariant failure; WinUI installs a fresh flat mapping over the same projection rows.

## Grouping and sorting

Runtime group headers contain three structured slots.
Each slot retains absence, raw text, a numeric year, a typed `MissingTrackValueKind`, or the `VariousArtists` marker.
The secondary slot of a Work group holds its composer, and that of an Album group holds its album artist.
An Album group without an album artist holds instead the artist that more than half of its tracks with an artist name, `VariousArtists` when no artist has such a majority, or the missing-artist value when none names one; only this slot holds `VariousArtists`.
Every grouping populates the primary slot with raw text, a numeric year, or a typed missing value before materializing a section.
Unknown group values and `VariousArtists` remain distinct semantic values rather than merging with an unrelated concrete value or becoming English inside runtime.
UIModel resolves the slots through feature presentation functions over `MessageCatalog`, which supplies the localized missing-value labels and Various Artists text, and frontend adapters own only markup and geometry.

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

GTK and WinUI use their canonical startup locale for textual ordering. TUI
starts with that locale and may replace the injected ordering policy when its
locale preference changes; `ViewService` then rebuilds active projections and
publishes resets under the new policy.
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
RecordingYear compares the date's year; RecordingDate compares the full `(year, month, day)` tuple, retaining zero for unknown components.
Present dates precede absent dates in either direction; descending reverses values, not the presence rule.
This ordering is separate from query-literal precision comparison.
Rows materialize the date scalar and compact credit projections, never full credit lists.

Normalization deduplicates visible/redundant fields and removes credit category fields from redundant-field suppression: a primary-name heading cannot replace a row's additional-entry count.

### Credit projections

Conductor, Ensemble, and Soloist columns retain the first name plus a localized `+N` for additional entries, counting duplicates; empty categories display absence.
Rows own only first-name text and entry count, not vectors or joined lists.
Sorting uses the first name with the existing missing-text ordering and tie rules.
Conductor and Ensemble grouping likewise uses that first name; this adds no Soloist/Performer grouping key or preset.
Group headings show the primary name, never a per-track `+N`, and grouped category rows retain their compact count-bearing value.

Member predicates search every selected name, not just this first-entry projection.
A track matching its second conductor can remain grouped under its first; details reveal every name, kind, and optional role in canonical order.
Reordering within a kind deliberately changes the primary projection; there is no separate primary flag.
Detail/form mixed state compares complete per-kind lists, including roles, order, and duplicate multiplicity, rather than these compact strings.
Full lists belong to owning focused/detail/editor snapshots, not per-frame transactions or table-row caches.

## Classical behavior

Work grouping merges recordings of the same `(composer, work)` into one section.
The classical presets keep recordings contiguous with this core sort chain, prefixed by their grouping concern where applicable:

```text
Composer -> Work -> RecordingYear -> Year -> Album -> Movement -> DiscNumber -> TrackNumber -> Title
```

RecordingYear separates recordings without treating partial dates as full sessions; unknown recording years remain after present years.
Year and Album remain subsequent boundaries, so recordings within the same year retain album-contiguous movement order.
Album precedes Movement so different performances do not interleave movement-by-movement.
Movement name is the visible row label, while movement number controls order.
All three classical presets show RecordingDate; none uses full RecordingDate as a preset sort term.

Classical Works groups by Work and suppresses composer/work fields already represented by its header.
Classical Composers groups by Composer and keeps Work visible because multiple works occur in the section.
Classical Conductors groups by Conductor and keeps Composer, Work, Ensemble, and Movement visible.

## Recommendation

Saved-List recommendation inspects successfully parsed local-filter variables with this priority:

| Filter signal                      | Recommended intent  |
| ---------------------------------- | ------------------- |
| Work                               | Classical Works     |
| Composer                           | Classical Composers |
| Sample rate, bit depth, or bitrate | Technical           |
| Tag                                | Tagging             |
| Genre or year                      | Albums              |
| Album artist                       | Artists             |
| Artist or album                    | Albums              |
| No recognized signal               | Albums              |

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
Changing the active presentation triggers a projection rebuild as defined by [track-list projection](../library/track-list-projection.md).

GTK identifies a list item by `TrackId`. As long as GTK holds the item, a reorder returns the same object so selection follows the track. GTK may inspect many items to find a selection or lay out a list; looking up an identity does not load or retain track metadata.
At bind, cells resolve and observe the row's display fields; views share cached `TrackRowObject` data and its memoized display strings.
Before notifying GTK of a projection change, the adapter refreshes affected bound rows and makes current fields available to explicit lookups during the notification.
A grouped structural reset may limit field refresh to the affected tracks when they are known. Reloading the fields invalidates memoized strings.
Each cell subscribes to `TrackListModel` playing changes once at setup, independently of its row's field subscription. At bind it applies the model's current playing state.

## Implementation map

- [`TrackPresentation.h`](../../../app/include/ao/rt/TrackPresentation.h) defines the spec and preset values.
- [`TrackPresentation.cpp`](../../../app/runtime/TrackPresentation.cpp) owns built-ins and normalization.
- [`TrackListProjection.cpp`](../../../app/runtime/projection/TrackListProjection.cpp) owns grouping, sorting, source-order stability, and ordering-policy replacement.
- [`TrackGroupHeadingPresentation.cpp`](../../../app/uimodel/library/presentation/TrackGroupHeadingPresentation.cpp) resolves runtime heading values through the shared catalog.
- UIModel presentation code under [`app/uimodel/library/presentation/`](../../../app/uimodel/library/presentation) owns catalog and recommendation adaptation.
- [`TrackListItem.cpp`](../../../app/linux-gtk/track/TrackListItem.cpp) weakly indexes native list items by track id; [`TrackListModel.cpp`](../../../app/linux-gtk/track/TrackListModel.cpp) adapts projection notifications and exposes now-playing state.
- [`TrackRowCache.cpp`](../../../app/linux-gtk/track/TrackRowCache.cpp) resolves display fields and prepares cached rows for publication; [`TrackColumnFactoryBuilder.cpp`](../../../app/linux-gtk/track/TrackColumnFactoryBuilder.cpp) binds cell fields and styles.

## Test map

- [`TrackPresentationTest.cpp`](../../../test/runtime/TrackPresentationTest.cpp) locks built-ins and normalization.
- Sorting and grouping tests under [`test/runtime/projection/`](../../../test/runtime/projection) lock compound groups and classical order.
- Presentation recommendation tests under [`test/uimodel/library/presentation/`](../../../test/uimodel/library/presentation) lock selection behavior.
- [`TrackListItemTest.cpp`](../../../test/linux-gtk/track/TrackListItemTest.cpp) protects GTK item identity through native-only ownership, disposal, and pool retirement.
- [`TrackListPublicationTest.cpp`](../../../test/linux-gtk/track/TrackListPublicationTest.cpp) checks field freshness during grouped reorder and filter entry notifications, and continued field delivery after a list-only restore.
- [`TrackListModelTest.cpp`](../../../test/linux-gtk/track/TrackListModelTest.cpp) checks GTK selection across reorder without loading unrelated fields, section mapping, and now-playing updates without structural changes.
- [`TrackRowCacheTest.cpp`](../../../test/linux-gtk/track/TrackRowCacheTest.cpp) protects cache invalidation and in-place field refresh.
- [`TrackColumnFactoryBuilderTest.cpp`](../../../test/linux-gtk/track/TrackColumnFactoryBuilderTest.cpp) checks bound cell updates, now-playing styles, and inline draft handling across updates and reuse.

## Related documents

- [Track preset reference](../../reference/presentation/track-preset.md)
- [Presentation text catalog](text-catalog.md)
- [Track model](../../reference/library/model/track.md)
- [Track field catalog](../../reference/library/model/track-field.md)
- [Presentation architecture](README.md)
- [Track expression architecture](../query/README.md)
- [Track filtering](track-filter.md)
- [List presentation preference](list-preference.md)
- [Track-column layout](track-column-layout.md)
