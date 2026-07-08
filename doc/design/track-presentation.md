# Track Presentation

A track presentation describes the *shape* of a track list: how rows are
grouped, how they are ordered, and which columns are shown. It is deliberately
separate from list *content* — see
[List Presentation Preferences](list-presentation-preferences.md) for how a
presentation is chosen and persisted per list. Column width behavior is covered
by [Track Column Widths](track-column-widths.md).

## The spec model

`rt::TrackPresentationSpec` (`app/include/ao/rt/TrackPresentation.h`) is the
complete, serializable description of one presentation:

| Field | Meaning |
|---|---|
| `id` | Stable identifier. Builtin ids are part of persisted session state. |
| `groupBy` | One `TrackGroupKey`, or `None` for a flat list. |
| `sortBy` | Ordered `TrackSortTerm` list applied within the whole view. |
| `visibleFields` | Row columns, in display order. |
| `redundantFields` | Columns suppressed because the group header already shows their value. |

`normalizeTrackPresentationSpec()` deduplicates field lists and defaults an
empty id to `kDefaultTrackPresentationId`.

Two preset types wrap the spec:

- `TrackPresentationPreset` — a builtin: spec plus UI `label` and
  `description` (static storage, `builtinTrackPresentationPresets()`).
- `CustomTrackPresentationPreset` — user-defined: spec plus `label` and the
  `basePresetId` it was derived from. Custom presets are the escape valve for
  taste variations (descending years, Tags in a browse view, ...) so the
  builtin set can stay small.

## Builtin presets

Builtins are defined in `app/runtime/TrackPresentation.cpp` and answer one
user intent each. The vector order is the menu order in both frontends and is
grouped into tiers:

| Tier | Id | Label | Shape |
|---|---|---|---|
| Daily listening | `library` | Library | Flat; album-artist → album → disc → track order. **Default.** |
| | `songs` | Songs | Flat; title → artist → album order. |
| | `albums` | Albums | Grouped by album; track-oriented columns. |
| | `artists` | Artists | Grouped by **album artist**; discography ordering (year before album). |
| Browsing | `performers` | Performers | Grouped by **track artist**, including featured guests. |
| | `genres` | Genres | Grouped by genre; album-artist discography ordering inside. |
| | `years` | Years | Grouped by year; album-artist discography ordering inside. |
| Classical | `classical-composers` | Classical: Composers | Grouped by composer; work-oriented columns. |
| | `classical-works` | Classical: Works | Grouped by work; movement-oriented columns. |
| Maintenance | `tagging` | Tagging | Flat curation list with raw disc/track numbers and Tags. |
| | `technical` | Technical | Flat file-inspection list (codec summary, bitrate, size, path). |

Design decisions behind the set:

- **The default is `library`, and its first sort key is `AlbumArtist`.** A
  flat all-tracks view sorted by track artist would tear various-artist
  compilations apart; album-artist-first keeps every album contiguous, which
  is the point of an album-order default.
- **"Artists" means album artist.** Users asking for an artist view expect a
  discography that is not fragmented by featurings and compilations. The
  track-artist grouping still exists as `performers` for the "who sang on
  this" intent.
- **Tags appear only in `tagging`.** Tags are curation data, not listening
  data; as a variable-length list column they are the first thing to crowd a
  row. Users who want them elsewhere derive a custom preset.
- **`tagging` shows raw `DiscNumber` and `TrackNumber` columns** instead of
  the synthetic `DisplayTrackNumber`. The display formatter hides the disc
  part when `discTotal <= 1`, which is exactly the kind of metadata defect a
  curation view must expose rather than paper over.
- **`technical` uses the synthetic `TechnicalSummary` column** (codec ·
  sample rate · bit depth · bitrate) plus separate `FileSize` and
  `FilePath` columns, rather than individual technical columns —
  horizontal space is the constraint in this view.
- **Genre/year groups order their contents as discographies**
  (`AlbumArtist → Year → Album → ...`), not as timelines, so one artist's
  albums stay together inside a genre or year section.
- Classical sort order (why `Album` precedes `Movement`, movement-number
  sorting) is owned by
  [Classical Work / Movement](classical-work-movement.md).

## Grouping semantics

Grouping is implemented by `LiveTrackListProjection`
(`app/runtime/projection/LiveTrackListProjection.cpp`). Two group keys are
compound even though the spec names a single field:

- `TrackGroupKey::Album` groups by **(album artist, album)** so identically
  named albums ("Greatest Hits") do not merge.
- `TrackGroupKey::Work` groups by **(composer, work)** so identically named
  works ("Symphony No. 5") do not merge.

Section headers carry a `primaryText` and, for compound keys, a
`secondaryText` (the album artist for album groups, the composer for work
groups). Both frontends render the secondary text, which is what makes the
`redundantFields` entries for those presets safe: a column belongs in
`redundantFields` only when the group header already presents its value.

If a spec arrives with a `groupBy` but an **empty** `redundantFields`,
`LiveTrackListProjection::setPresentation()` borrows the redundant fields of the
first builtin preset with the same `groupBy`. Custom specs that want no
suppression under a grouping must therefore say so by listing visible fields
explicitly rather than relying on an empty redundant list.

## Sorting

`TrackSortField` is a deliberate subset of `TrackField`: only metadata that
is resident in the in-memory track snapshot is sortable. Two consequences:

- `TrackSortField::Movement` compares `movementNumber` (numeric), not the
  movement name string, despite its name.
- `FileSize` and `ModifiedTime` are displayable but **not sortable**: both
  are resolved per track through the file-manifest reader, so making them
  sort keys means a manifest lookup per row across the whole view. The
  `technical` preset therefore falls back to a metadata-only sort order.
  Extending the sort model to manifest-sourced fields is deferred, deliberate
  follow-up work, not an oversight.

## Selection and recommendation

`uimodel::TrackPresentationCatalog` merges builtins and custom presets into
menu items (builtins in tier order, separator, customs, "Create Custom
View"). The tier structure is expressed through ordering only; dedicated
per-tier separators would need a separator concept in the TUI navigation
model and are deferred.

`uimodel::recommendPresentation()` picks a presentation for a smart list from
the variables in its filter expression, in priority order:

| Filter mentions | Recommended preset |
|---|---|
| `$work` | `classical-works` |
| `$composer` | `classical-composers` |
| `@sampleRate` / `@bitDepth` / `@bitrate` | `technical` |
| `#tag` | `tagging` |
| `$genre` / `$year` | `albums` |
| `$albumArtist` | `artists` |
| `$artist` / `$album` | `albums` |
| nothing / unparsable | `albums` |
