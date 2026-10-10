---
id: library.track-model
---
# Track model

## Scope and version

This reference enumerates the current logical track values owned by library storage and core read models.
Physical byte placement belongs to [library database version 9](../storage/database.md), and portable names belong to [library YAML version 7](../format/yaml.md).
Application-facing ids, presentation capabilities, sort/group mappings, completion flags, and query bridges belong to the [runtime track field catalog](track-field.md).

Zero numeric values and invalid ids represent unknown or absent values unless a narrower contract states otherwise.
The signed storage type of duration does not define negative values as meaningful playback lengths.
Before staging dictionary or resource changes, `TrackBuilder` cold-data validation rejects negative durations with `InvalidInput` and values above `2147483647` ms with `ValueTooLarge`.
Accepted durations are stored without narrowing loss; the [YAML technical-property rules](../format/yaml.md#technical-properties) describe how import reports an unrepresentable value.

## Code boundary

The persisted model belongs to the **core libraries** layer in the [system architecture](../../../system/overview.md) and the [library architecture](../../../system/library/structure.md): core builders/views live under `include/ao/library/` and `lib/library/`.
Runtime and presentation consumers adapt these values without changing their storage authority.

## Curated metadata

| Field | Logical type | Persisted representation |
|---|---|---|
| Title | Text | Inline UTF-8 hot title |
| Artist | Text | Hot `DictionaryId` |
| Album | Text | Hot `DictionaryId` |
| Album artist | Text | Hot `DictionaryId` |
| Genre | Text | Hot `DictionaryId` |
| Composer | Text | Hot `DictionaryId` |
| Work | Text | Work-block `DictionaryId` |
| Movement | Text | Work-block `DictionaryId` |
| Recording date | Optional partial calendar date | Performance prefix: year, month, day |
| Credits | Canonical list of name, kind, optional role | Performance tail: name and role dictionary ids in four kind segments |
| Year | Unsigned 16-bit | Hot scalar |
| Disc number | Unsigned 16-bit | Cold scalar |
| Disc total | Unsigned 16-bit | Cold scalar |
| Track number | Unsigned 16-bit | Cold scalar |
| Track total | Unsigned 16-bit | Cold scalar |
| Movement number | Unsigned 16-bit | Work-block scalar |
| Movement total | Unsigned 16-bit | Work-block scalar |

Work and performance are independently optional storage domains.
The work block is absent when both ids and both movement numbers are zero; performance is absent when RecordingDate is absent and every credit segment is empty.
A date-only or credits-only performance therefore needs no work block.
Movement is a leaf value inside the work block; composer remains hot, and application grouping and sorting rules belong to the runtime field and presentation contracts.
Credits are the only stored authority for Conductor, Ensemble, Soloist, and Performer participants.
Category fields are projections, not scalar storage mirrors; Composer, Artist, and AlbumArtist remain separate concepts.
Neither block establishes a musical entity or a unique recording identity.
All curated text is scalar-valid UTF-8 in NFC: title is normalized before inline persistence, and dictionary-backed values are normalized before interning.

## Tags and custom metadata

Tags are a set of shared-dictionary ids persisted in the hot record, plus a 32-bit bloom filter.
The builder suppresses duplicate names, while serialization records resolved ids; tag order has no public semantic meaning.
The serialized tag-id array contains complete membership, and the bloom filter is only an acceleration aid.

Custom metadata is an ordered key/value collection in the cold custom block.
Keys are `DictionaryId` values and values are inline scalar-valid UTF-8 NFC byte ranges.
An absent block and an empty collection both expose no entries.

Tag names and custom-metadata keys are normalized to NFC before dictionary identity lookup.
Canonically equivalent spellings therefore share one dictionary id while the persisted NFC spelling remains suitable for display.

`MetadataPatch` may set or clear curated metadata and custom metadata.
Tag additions/removals use the separate tag command contract in [library access and mutation](../../../system/library/mutation.md).

## Technical properties

| Field | Logical type | Units or values | Persisted location |
|---|---|---|---|
| `duration` | Signed 32-bit duration | Milliseconds | Cold header |
| `bitrate` | Unsigned 32-bit | Bits per second | Cold header |
| `sample-rate` | Unsigned 32-bit | Hertz | Hot header |
| `codec` | `AudioCodec` | See codec table | Hot header |
| `channels` | Unsigned 8-bit | Channel count | Cold header |
| `bit-depth` | Unsigned 8-bit | Bits per sample | Hot header |
| `file-path` | Text path | Music-root-relative URI | Cold URI |
| `file-size` | Unsigned 64-bit | Bytes | Manifest |
| `modified-time` | Optional portable instant | Signed 64-bit Unix/POSIX-epoch seconds plus nanoseconds below `1000000000`; absent when unknown | Manifest |

Display projects `modified-time` without changing it: a present instant renders in the local zone, or UTC when that zone is unavailable; an instant outside the display calendar's range renders empty, and an absent value renders empty.

## Codec values

`AudioCodec` identifies the audio encoding, not the container or filename extension.

| Name | Raw value | Display/YAML name |
|---|---:|---|
| `Unknown` | `0` | `UNKNOWN` |
| `Flac` | `1` | `FLAC` |
| `Alac` | `2` | `ALAC` |
| `Wav` | `3` | `WAV` |
| `Aac` | `128` | `AAC` |
| `Mp3` | `129` | `MP3` |
| `Opus` | `130` | `Opus` |

`audioCodecName` produces the canonical name, `parseAudioCodecName` parses names case-insensitively, and `audioCodecFromStorage` converts unknown raw values to `Unknown`.

## Cover art

A track has an ordered sequence of `CoverArt` values.
Each value contains a nonzero `ResourceId` and a `PictureType`; `ResourceStore` holds a descriptor naming the image by digest, and identical content deduplicates to one id.

`CoverArtProxy::primary()` returns the first `FrontCover`, otherwise the first entry, otherwise no value.
The builder preserves insertion order and supports adding by bytes, by existing resource id, or by a declared descriptor, indexed erase, and complete clearing.
A scan replaces the whole sequence from the file it read, and a full import restores one from a transfer document; nothing else writes it.

`PictureType` uses the APIC/FLAC numeric vocabulary from `0` (`Other`) through `20` (`PublisherLogo`).
Unknown imported numeric roles normalize to `Other`.

## Validation rules

Builder serialization rejects values or aggregate records that exceed their encoded widths.
It rejects malformed UTF-8 and checks encoded collection and complete-record bounds before dictionary, Resource, or Track mutation.
Inline text sizes are checked in their normalized NFC representation; credit names and roles are validated directly before interning, where dictionary admission owns NFC.
Every dictionary and resource reference uses its strongly typed id; zero is the only invalid id sentinel.
URI and collection layout bounds are enforced before store writes.
The music-root-relative URI is filesystem identity: it is neither Unicode-normalized nor case-folded by the text admission path.

### Metadata input values

Core supplies independent input-value contracts in [`RecordingDate.h`](../../../../include/ao/library/RecordingDate.h) and [`Credits.h`](../../../../include/ao/library/Credits.h), consumed by the persisted builder/view surface.
`Credit` owns name/role text; `CreditView` borrows it. Both carry a fixed `CreditKind`.
`MetadataBuilder::credits()` replaces the whole list from owning or borrowed input; an empty list clears credits without clearing RecordingDate.
Both setters copy descriptors, not the supplied strings: source text must survive through preparation, even for owning inputs.
Immutable prepared values own every id and byte needed by serialization and no longer depend on the builder or source text.
`TrackView::work()` and `performance()` borrow their cold blocks.
`PerformanceView::credits()` spans all physical `TrackCreditEntry` values; `credits(kind)` spans one segment. These entries contain dictionary ids, not logical kinds; consumers reconstruct kinds by visiting the four segments.
The spans share the record/buffer or transaction lifetime and do not materialize logical lists.
Complete reconstruction retains every field and canonical entry; hot reconstruction never fetches cold metadata or credits.

`RecordingDate` carries year, month, and day with zero components beyond its precision; all-zero means absent.
A present value is a valid partial proleptic Gregorian date in years 0001 through 9999.
Its parser accepts only canonical present `YYYY`, `YYYY-MM`, and `YYYY-MM-DD` literals, without trimming or accepting empty input; formatting preserves precision and formats absence empty.
An editing adapter must handle surrounding whitespace and explicit clear separately rather than weakening canonical query-literal admission.
Exact equality includes precision.
A day without a month, components without a year, invalid month lengths, and invalid leap days are rejected rather than corrected.
This is when the performance was recorded, not release or composition date; it is never inferred from Year, album/work text, `DATE`, `YEAR`, or ID3 `TDRC`.
The existing Year field and its generic file-tag mappings remain independent.
The separately named literal-precision comparison compares only components supplied by its present literal, retains zero for missing stored components, and returns no comparison for an absent stored value.
The [predicate reference](../../query/predicate-language.md#recording-date-predicates) owns query operators; those partial comparisons must not replace exact precision-sensitive equality for aggregation, edits, or serialization.

`normalizeCredits` accepts borrowed or owning input and returns owning canonical values.
It trims only space, tab, carriage return, line feed, form feed, and vertical tab at name/role boundaries, validates scalar UTF-8, and stores NFC.
A blank authored name or invalid kind rejects the complete input; a blank role becomes absent, represented by an empty string.
NBSP and other non-ASCII whitespace remain text. No case folding or heuristic name splitting occurs.
Canonical order is Conductor, Ensemble, Soloist, Performer, with stable within-kind order.
Duplicates, repeated names/roles, and the same name in different kinds remain distinct entries.
Equality, NoOp, stored/editor snapshots, Undo, and interchange compare normalized name, kind, role, duplicate multiplicity, and within-kind order. Reordering within a kind is a real edit.
Role is descriptive free text: it never determines kind and is not independently queryable, sortable, or groupable.
Names identify supplied text, not entities; there is no primary flag, person registry, deduplication, or cross-entry synchronization.

Builder preflight independently checks every kind, trimmed nonempty name, name/role UTF-8, partial date, counters, and full serialized record bounds before interning or Resource operations, even when callers pass unnormalized borrowed media input.
It uses wide checked size arithmetic before narrowing, including retained blocks and URI, not only a replacement list.
Preparation trims, stably groups, and interns credit text; dictionary admission supplies NFC.
Names receive nonzero dictionary ids and absent roles use id zero; kind labels consume no dictionary entries.
A failure after interning begins fails the enclosing write and aborts its root transaction: callers cannot catch it and continue committing that transaction, and transaction-private dictionary text is never published before root commit.
This does not promise allocation-free cold validation or per-call rollback.

The [member predicates](../../query/predicate-language.md) search category names or all credit names, not roles.
First-entry sorting/grouping and compact counts are distinct [presentation projections](../../../system/presentation/track-presentation.md#credit-projections).
Runtime writes use the [typed scoped replacement](../../../system/presentation/metadata-editing.md#credits), never category preview text.

[`RecordingDateTest.cpp`](../../../../test/unit/library/RecordingDateTest.cpp) and [`CreditsTest.cpp`](../../../../test/unit/library/CreditsTest.cpp) specify input contracts; [`TrackPerformanceTest.cpp`](../../../../test/unit/library/TrackPerformanceTest.cpp) and [`TrackPerformanceValidationTest.cpp`](../../../../test/unit/library/TrackPerformanceValidationTest.cpp) specify persistence, bounds, and malformed/open behavior.

## Compatibility and versioning

Changing a persisted type, codec value, block meaning, or field width requires a library format version increment.
Database version 9 gates the work/performance split with RecordingDate and segmented Credits; version 8 gated the portable manifest modification-time instant, and version 7 made scalar-valid UTF-8 NFC an admission invariant for Track text and its dictionary references.
Version 8 and earlier libraries are rejected rather than converted, relabeled, backfilled, or deleted automatically.
Only the exact layouts defined by the database version 9 and YAML version 7 references are accepted; alternative layouts with those version labels are unsupported.
There is no dual reader, layout autodetection, or migration.
The YAML version 7 importer rejects version-6 documents.
Portable YAML names and runtime field ids are separate compatibility surfaces owned by their respective references.

## Implementation authority

- [`TrackBuilder.h`](../../../../include/ao/library/TrackBuilder.h) and [`TrackView.h`](../../../../include/ao/library/TrackView.h) define the core logical surface.
- [`TrackLayout.h`](../../../../include/ao/library/TrackLayout.h) defines persisted representations.
- [`RecordingDate.h`](../../../../include/ao/library/RecordingDate.h) and [`Credits.h`](../../../../include/ao/library/Credits.h) own canonical partial-date and credit input rules.
- [`CoverArt.h`](../../../../include/ao/library/CoverArt.h) defines picture roles and primary selection.
- [`AudioCodec.h`](../../../../include/ao/AudioCodec.h) defines codec values and conversion helpers.

## Test authority

- Track builder/view/layout tests under [`test/unit/library/`](../../../../test/unit/library) lock serialization, validation, covers, tags, and custom metadata.

## Related documents

- [Resource descriptors](../../resource/blob.md)
- [Supported audio files](../../media/audio-file.md)
- [Library YAML format](../format/yaml.md)
- [Runtime track field catalog](track-field.md)
- [Track sources](../../../system/library/track-source.md)
- [Track-list projection](../../../system/library/track-list-projection.md)
