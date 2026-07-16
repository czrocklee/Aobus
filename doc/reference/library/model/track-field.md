---
id: library.track-field
type: reference
status: current
domain: library
summary: Enumerates runtime track fields, capabilities, sort and group mappings, query bridges, and persistence-facing enum values.
---
# Runtime track field catalog

## Scope and version

This reference defines the application-runtime `TrackFieldDefinition` catalog and its related `TrackField`, `TrackSortField`, and `TrackGroupKey` values.
The persisted logical values behind these fields belong to the [track model](track.md), while expression variables belong to the [predicate language](../../query/predicate-language.md).

The catalog is an application-facing adaptation of library data.
It is not a second persistence model and does not define query truth or presentation behavior.

## Code boundary

The catalog belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md), at the seam described by the [library](../../../architecture/library.md), [track expression](../../../architecture/track-expression.md), and [presentation](../../../architecture/presentation.md) architectures.
Its public values live in `app/include/ao/rt/TrackField.h` and its definitions live in `app/runtime/TrackField.cpp`; UIModel and frontends consume the catalog without redefining it.

## Surface

Every current `TrackField` is presentable.
The tables list the additional capabilities and exact bridge values.

### Curated metadata

| Raw | Field | Id | Label | Kind | Editable | Sort key | Group key | Query variable | Value completion |
|---:|---|---|---|---|:---:|---|---|---|:---:|
| 0 | `Title` | `title` | Title | Text | Yes | `Title` | — | `$title` | No |
| 1 | `Artist` | `artist` | Artist | Text | Yes | `Artist` | `Artist` | `$artist` | Yes |
| 2 | `Album` | `album` | Album | Text | Yes | `Album` | `Album` | `$album` | Yes |
| 3 | `AlbumArtist` | `album-artist` | Album Artist | Text | Yes | `AlbumArtist` | `AlbumArtist` | `$albumArtist` | Yes |
| 4 | `Genre` | `genre` | Genre | Text | Yes | `Genre` | `Genre` | `$genre` | Yes |
| 5 | `Composer` | `composer` | Composer | Text | Yes | `Composer` | `Composer` | `$composer` | Yes |
| 6 | `Conductor` | `conductor` | Conductor | Text | Yes | `Conductor` | `Conductor` | `$conductor` | Yes |
| 7 | `Ensemble` | `ensemble` | Ensemble | Text | Yes | `Ensemble` | `Ensemble` | `$ensemble` | Yes |
| 8 | `Work` | `work` | Work | Text | Yes | `Work` | `Work` | `$work` | Yes |
| 9 | `Movement` | `movement` | Movement | Text | Yes | `Movement` | — | `$movement` | Yes |
| 10 | `Soloist` | `soloist` | Soloist | Text | Yes | `Soloist` | — | `$soloist` | Yes |
| 11 | `Year` | `year` | Year | Number | Yes | `Year` | `Year` | `$year` | No |
| 12 | `DiscNumber` | `disc-number` | Disc | Number | Yes | `DiscNumber` | — | `$discNumber` | No |
| 13 | `DiscTotal` | `disc-total` | Total Discs | Number | Yes | — | — | `$discTotal` | No |
| 14 | `TrackNumber` | `track-number` | Track | Number | Yes | `TrackNumber` | — | `$trackNumber` | No |
| 15 | `TrackTotal` | `track-total` | Total Tracks | Number | Yes | — | — | `$trackTotal` | No |
| 16 | `MovementNumber` | `movement-number` | Movement No. | Number | Yes | `Movement` | — | `$movementNumber` | No |
| 17 | `MovementTotal` | `movement-total` | Total Movements | Number | Yes | — | — | `$movementTotal` | No |

All rows in this table have category `Metadata`.
Movement name and movement number intentionally map to the same `Movement` sort key; projection ordering uses the number while the name remains display text.

### Tags and technical fields

| Raw | Field | Id | Label | Category | Kind | Sort key | Query variable |
|---:|---|---|---|---|---|---|---|
| 18 | `Duration` | `duration` | Duration | Technical | Duration | `Duration` | `@duration` |
| 19 | `Tags` | `tags` | Tags | Tag | TagList | — | — |
| 20 | `FilePath` | `file-path` | File Path | Technical | FilePath | — | — |
| 21 | `Codec` | `codec` | Codec | Technical | TechnicalText | — | `@codec` |
| 22 | `SampleRate` | `sample-rate` | Sample Rate | Technical | TechnicalText | — | `@sampleRate` |
| 23 | `Channels` | `channels` | Channels | Technical | TechnicalText | — | `@channels` |
| 24 | `BitDepth` | `bit-depth` | Bit Depth | Technical | TechnicalText | — | `@bitDepth` |
| 25 | `Bitrate` | `bitrate` | Bitrate | Technical | TechnicalText | — | `@bitrate` |
| 26 | `FileSize` | `file-size` | File Size | Technical | TechnicalText | — | — |
| 27 | `ModifiedTime` | `modified-time` | Modified | Technical | TechnicalText | — | — |

These fields are not editable and do not support metadata-value completion.
Tags use dynamic `#name` variables rather than one fixed query-variable mapping.

### Synthetic fields

| Raw | Field | Id | Label | Kind | Meaning |
|---:|---|---|---|---|---|
| 28 | `DisplayTrackNumber` | `display-track-number` | Track # | TechnicalText | Formatted disc/track position |
| 29 | `TechnicalSummary` | `technical-summary` | Technical | TechnicalText | Combined codec, rate, depth, and bitrate text |
| 30 | `Quality` | `quality` | Quality | TechnicalText | Derived quality label |

All three rows have category `Synthetic`, set the synthetic capability, and are neither editable, sortable, groupable, value-completable, nor filter-mapped.

### Sort fields

| Raw | `TrackSortField` |
|---:|---|
| 0 | `Artist` |
| 1 | `Album` |
| 2 | `AlbumArtist` |
| 3 | `Genre` |
| 4 | `Composer` |
| 5 | `Conductor` |
| 6 | `Ensemble` |
| 7 | `Work` |
| 8 | `Movement` |
| 9 | `Soloist` |
| 10 | `Year` |
| 11 | `DiscNumber` |
| 12 | `TrackNumber` |
| 13 | `Title` |
| 14 | `Duration` |

### Group keys

| Raw | `TrackGroupKey` |
|---:|---|
| 0 | `None` |
| 1 | `Artist` |
| 2 | `Album` |
| 3 | `AlbumArtist` |
| 4 | `Genre` |
| 5 | `Composer` |
| 6 | `Conductor` |
| 7 | `Ensemble` |
| 8 | `Work` |
| 9 | `Year` |

## Validation rules

- The definition array contains exactly one row for every `TrackField` value.
- Field ids and enum values are unique.
- A sortable field has a valid sort mapping, and a non-sortable field has none.
- A groupable field has a valid group mapping, and a non-groupable field has none.
- Every field carrying the value-completion flag has a resolvable typed bridge to a dictionary-backed query field; the runtime support predicate and vocabulary service enforce and derive the same set from those definitions.
- An absent typed query-field mapping means the application field cannot be converted directly into one fixed filter variable.
- Every typed runtime bridge resolves to one core `QueryVariableDescriptor`, and no two runtime fields claim the same scalar query field.
- Every canonical core `$` or `@` query field has one runtime bridge except `$coverArt`, whose resource identity has no application `TrackField` counterpart.
- Canonical query-variable text is derived from the core descriptor rather than stored as a second string in the runtime catalog.
- `trackFieldFromId` is case-sensitive and returns no value for unknown, empty, or differently cased ids.
- Invalid enum inputs to lookup helpers return empty or absent values rather than indexing the catalog.

## Compatibility and versioning

Track-field ids appear in application-facing state and must not be renamed without updating persistence, transfer, presentation, and tests.
Enum raw values are serialized by current configuration surfaces; reordering or inserting values requires an explicit compatibility migration.

Query-variable text is owned by the predicate language.
Changing a typed mapping requires both catalogs and their exhaustive tests to agree; it does not authorize a new query variable by itself.

Adding a field requires coordinated decisions about persistence source, projection materialization, editing, sorting, grouping, completion, filtering, display formatting, and saved configuration.

## Examples

- `TrackField::Artist` is editable, sortable, groupable, value-completable, and maps to `$artist`.
- `TrackField::MovementNumber` is editable and maps to `$movementNumber`, while its presentation sort key is `TrackSortField::Movement`.
- `TrackField::Codec` maps to the existing `@codec` query variable without becoming value-completable.
- `TrackField::Tags` is presentable but has no fixed query-variable bridge because each tag is a distinct `#name` predicate.
- `TrackField::Quality` is a synthetic presentation field with no library edit or query mapping.

## Implementation authority

- [`TrackField.h`](../../../../app/include/ao/rt/TrackField.h) defines enum and catalog shapes.
- [`TrackField.cpp`](../../../../app/runtime/TrackField.cpp) defines every row and lookup helper.
- [`FieldCatalog.h`](../../../../include/ao/query/FieldCatalog.h) and [`FieldCatalog.cpp`](../../../../lib/query/FieldCatalog.cpp) define the core typed descriptors.
- [`CompletionService.cpp`](../../../../app/runtime/completion/CompletionService.cpp) derives source-preserving dictionary-field frequencies and field materialization for the value-completable subset.

## Test authority

- [`TrackFieldTest.cpp`](../../../../test/unit/runtime/TrackFieldTest.cpp) locks catalog completeness, ids, labels, capabilities, exhaustive typed mappings, and invalid lookup behavior.
- [`TrackFieldPresentationPolicyTest.cpp`](../../../../test/unit/uimodel/library/presentation/TrackFieldPresentationPolicyTest.cpp) ensures every presentable field has UIModel column policy.
- [`CompletionServiceTest.cpp`](../../../../test/unit/runtime/completion/CompletionServiceTest.cpp) locks vocabulary support.
- [`GtkLayoutStateStoreTest.cpp`](../../../../test/unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp) protects persisted field enum compatibility at the GTK boundary.

## Related documents

- [Track model](track.md)
- [Predicate language](../../query/predicate-language.md)
- [Track-list presentation](../../../spec/presentation/track-presentation.md)
- [Track-field value completion](../../../spec/presentation/field-completion.md)
- [Track-list projection](../../../spec/library/projection/track-list.md)
