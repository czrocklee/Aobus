---
id: library.track-model
type: reference
status: current
domain: library
summary: Enumerates persisted track metadata, technical properties, codec values, cover entries, tags, and custom metadata.
---
# Track model

## Scope and version

This reference enumerates the current logical track values owned by library storage and core read models.
Physical byte placement belongs to [library database version 7](../storage/database.md), and portable names belong to [library YAML version 5](../format/yaml.md).
Application-facing ids, presentation capabilities, sort/group mappings, completion flags, and query bridges belong to the [runtime track field catalog](track-field.md).

Zero numeric values and invalid ids represent unknown or absent values unless a narrower contract states otherwise.

## Code boundary

The persisted model belongs to the **core libraries** layer in the [system architecture](../../../architecture/system-overview.md) and the [library architecture](../../../architecture/library.md): core builders/views live under `include/ao/library/` and `lib/library/`.
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
| Conductor | Text | Classical-block `DictionaryId` |
| Ensemble | Text | Classical-block `DictionaryId` |
| Work | Text | Classical-block `DictionaryId` |
| Movement | Text | Classical-block `DictionaryId` |
| Soloist | Text | Classical-block `DictionaryId` |
| Year | Unsigned 16-bit | Hot scalar |
| Disc number | Unsigned 16-bit | Cold scalar |
| Disc total | Unsigned 16-bit | Cold scalar |
| Track number | Unsigned 16-bit | Cold scalar |
| Track total | Unsigned 16-bit | Cold scalar |
| Movement number | Unsigned 16-bit | Classical-block scalar |
| Movement total | Unsigned 16-bit | Classical-block scalar |

The classical block is absent when all five ids and both movement numbers are zero.
Movement is a leaf value inside the classical block; application grouping and sorting rules belong to the runtime field and presentation contracts.
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
Tag additions/removals use the separate tag command contract in [library access and mutation](../../../spec/library/runtime/mutation.md).

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
| `modified-time` | Unsigned 64-bit | Filesystem timestamp count | Manifest |

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
It rejects malformed UTF-8 text and applies size limits to the normalized NFC representation before any dictionary or Track mutation.
Every dictionary and resource reference uses its strongly typed id; zero is the only invalid id sentinel.
URI and collection layout bounds are enforced before store writes.
The music-root-relative URI is filesystem identity: it is neither Unicode-normalized nor case-folded by the text admission path.

## Compatibility and versioning

Changing a persisted type, codec value, block meaning, or field width requires a library format version increment.
Database version 7 also makes scalar-valid UTF-8 NFC a semantic admission invariant for persisted Track text and its dictionary references; version 6 is rejected rather than normalized implicitly on open.
Portable YAML names and runtime field ids are separate compatibility surfaces owned by their respective references.

## Implementation authority

- [`TrackBuilder.h`](../../../../include/ao/library/TrackBuilder.h) and [`TrackView.h`](../../../../include/ao/library/TrackView.h) define the core logical surface.
- [`TrackLayout.h`](../../../../include/ao/library/TrackLayout.h) defines persisted representations.
- [`CoverArt.h`](../../../../include/ao/library/CoverArt.h) defines picture roles and primary selection.
- [`AudioCodec.h`](../../../../include/ao/AudioCodec.h) defines codec values and conversion helpers.

## Test authority

- Track builder/view/layout tests under [`test/unit/library/`](../../../../test/unit/library/) lock serialization, validation, covers, tags, and custom metadata.

## Related documents

- [Resource descriptors](../../resource/blob.md)
- [Supported audio files](../../media/audio-file.md)
- [Library YAML format](../format/yaml.md)
- [Runtime track field catalog](track-field.md)
- [Track sources](../../../spec/library/source/track-source.md)
- [Track-list projection](../../../spec/library/projection/track-list.md)
