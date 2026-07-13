---
id: library.yaml-format
type: reference
status: current
domain: library
summary: Defines version 1 of the portable YAML library interchange format.
---
# Library YAML format

## Scope and version

This reference defines the exact version 1 YAML surface emitted by `LibraryYamlExporter` and accepted by `LibraryYamlImporter`.
It owns field names, node kinds, scalar widths, accepted values, producer omissions, and compatibility behavior.

Transfer modes, restore and merge behavior, atomicity, reports, previews, and change publication belong to the [library YAML transfer specification](../../../spec/library/runtime/yaml-transfer.md).
CLI commands and output conventions belong to the [CLI command reference](../../cli/command.md).

## Code boundary

This exact interchange surface belongs to the **application runtime** boundary in the [system architecture](../../../architecture/system-overview.md).
Producer and consumer code lives under `ao::rt::library`; the format translates core Library values but remains independent of the host-local `ao::library` storage layout.

## Document root

The root is a map with this shape:

```yaml
version: 1
libraryId: 123e4567-e89b-12d3-a456-426614174000
export_mode: full
library:
  tracks: []
  lists: []
```

| Field | Producer | Importer | Type and values |
|---|---|---|---|
| `version` | Always emitted as `1`. | Required. | Unsigned 32-bit integer; only `1` is supported. |
| `libraryId` | Always emitted. | Optional. | Canonical UUID text with hexadecimal digits and hyphens in `8-4-4-4-12` grouping; letter case is ignored. |
| `export_mode` | Always emitted. | Optional; omission means `full`. | `delta`, `metadata`, `full`, or `listOnly`; `minimum` is accepted as a legacy alias for `delta`. |
| `library` | Always emitted. | Required. | Map. |
| `library.tracks` | Emitted as a sequence except in `listOnly`. | Optional; ignored when `export_mode` is `listOnly`. | Sequence of track maps. |
| `library.lists` | Always emitted as a sequence. | Optional. | Sequence of list maps. |

Unknown fields are ignored at the root, library, track, list, cover, and list-reference levels unless they replace a required field with the wrong node kind.

## Track records

Each track is a map.

| Field | Required on input | Type | Producer behavior |
|---|---|---|---|
| `id` | No. | Unsigned 32-bit integer. | Always emitted; nonzero producer-local track identity. |
| `uri` | Yes. | Non-empty string. | Always emitted as the library-root-relative manifest URI. |
| Metadata fields | No. | String or unsigned 16-bit integer according to the tables below. | Mode-dependent. |
| `custom` | No. | Map of string keys to scalar string values. | Emitted only when non-empty. |
| `tags` | No. | Sequence of scalar strings. | Emitted only when non-empty. |
| `covers` | No. | Sequence of cover maps. | Mode-dependent; an empty sequence is meaningful. |
| Technical fields | No. | Scalar values from the table below. | Emitted only in `full`. |
| `fileSize` | No. | Unsigned 64-bit integer. | Emitted in `full`; `0` when no manifest row exists. |
| `mtime` | No. | Unsigned 64-bit integer. | Emitted in `full`; `0` when no manifest row exists. |

Track `id` values need not match target-library IDs.
Duplicate nonzero track IDs in one payload are rejected; `0` and omitted IDs create no ID mapping for list references.

The importer replaces backslashes with forward slashes and applies lexical path normalization to track and URI-reference values before manifest matching.
The producer emits root-relative paths, but version 1 import currently accepts any non-empty lexically normalized path.

### Text metadata

The following fields are scalar strings:

| Field | Field | Field |
|---|---|---|
| `title` | `artist` | `album` |
| `album-artist` | `genre` | `composer` |
| `conductor` | `ensemble` | `work` |
| `movement` | `soloist` | |

### Numeric metadata

The following fields are unsigned 16-bit integers:

| Field | Field | Field |
|---|---|---|
| `year` | `track-number` | `track-total` |
| `disc-number` | `disc-total` | `movement-number` |
| `movement-total` | | |

Producer field names come from `rt::trackFieldId()` and use hyphens rather than camel case.

### Technical properties

| Field | Type | Units or accepted values |
|---|---|---|
| `duration` | Unsigned 32-bit integer. | Milliseconds. |
| `bitrate` | Unsigned 32-bit integer. | Bits per second. |
| `sample-rate` | Unsigned 32-bit integer. | Hertz. |
| `codec` | String. | Case-insensitive `UNKNOWN`, `FLAC`, `ALAC`, `WAV`, `AAC`, or `MP3`. |
| `channels` | Unsigned 8-bit integer. | Channel count. |
| `bit-depth` | Unsigned 8-bit integer. | Bits per sample. |

An unrecognized codec string is tolerated and leaves the import baseline codec unchanged.
For a new track with no baseline, that value is `Unknown`.

### Cover records

`covers` is an ordered sequence of maps:

```yaml
covers:
  - type: 3
    data: iVBORw0KGgo=
```

| Field | Required | Type |
|---|---|---|
| `type` | Yes. | Unsigned 32-bit integer. Values `0` through `20` map to the APIC/FLAC picture-type vocabulary; larger values map to `Other` (`0`). |
| `data` | Yes. | Non-empty base64 scalar. |

Malformed or empty base64 rejects the complete import.
The producer may attach YAML anchors to first occurrences of shared image data and aliases to later occurrences; the importer resolves aliases before validation.

## List records

Each list is a map.

| Field | Required on input | Type and meaning |
|---|---|---|
| `id` | Yes. | Nonzero unique unsigned 32-bit payload identity. |
| `parentId` | No. | Unsigned 32-bit payload list identity; omission or `0` means root. |
| `name` | Yes. | Scalar string. |
| `description` | No. | Scalar string. |
| `filter` | No. | Scalar string; presence selects a smart list. |
| `tracks` | No. | Sequence of manual-list track references; considered only when `filter` is absent. |

The producer always emits `id`, `parentId`, and `name`.
It omits an empty description, emits `filter` for smart lists, and emits `tracks` only for a non-empty manual list.

Manual-list track references accept three forms:

```yaml
tracks:
  - 42
  - id: 42
  - uri: music/example.flac
```

A scalar or `id` map refers to a track record's payload `id`.
A `uri` map resolves through the target manifest after URI normalization.
When a map contains both `id` and `uri`, `id` takes precedence.

Normal exports use scalar ID references.
`listOnly` exports use `{uri: ...}` references so lists can attach to tracks already present under different target IDs.

Dangling parent, track-ID, and URI references are ignored.
Duplicate manual-list references collapse to their first resolved occurrence while preserving first-occurrence order.

## Validation rules

The importer rejects malformed YAML and the following structural violations with `FormatRejected`:

- a non-map root or `library` node;
- missing or unsupported `version`;
- an unknown `export_mode` other than the `minimum` alias;
- a malformed `libraryId`;
- non-sequence `tracks`, `lists`, `tags`, `covers`, or manual-list `tracks` nodes;
- non-map track, list, or cover records;
- missing or empty track `uri`;
- duplicate nonzero track IDs;
- missing, zero, or duplicate list IDs;
- missing list `name`;
- list-reference maps without `id` or `uri`;
- numeric values outside their target unsigned width;
- non-scalar metadata, technical, tag, custom-metadata, and required cover values;
- malformed or empty cover base64.

The observable failure and rollback contract is defined by the [transfer specification](../../../spec/library/runtime/yaml-transfer.md#failure-and-cancellation).

## Compatibility and versioning

Version 1 has these explicit compatibility behaviors:

- absent `export_mode` means `full`;
- `minimum` is accepted as `delta` but is never emitted;
- absent `libraryId`, `tracks`, and `lists` are accepted;
- unknown fields are ignored;
- unknown codec strings preserve the baseline codec;
- unknown cover types become `Other`;
- dangling list references are dropped;
- YAML anchors and aliases are accepted after resolution.

Changing a field name, node kind, scalar width, omission meaning, or compatibility rule requires a new format version or an explicitly tested backward-compatible extension.

## Examples

Full payload:

```yaml
version: 1
libraryId: 123e4567-e89b-12d3-a456-426614174000
export_mode: full
library:
  tracks:
    - id: 42
      uri: music/example.flac
      title: Example
      album-artist: Ensemble
      track-number: 1
      tags: [favorite]
      custom:
        mood: focused
      covers: []
      duration: 180000
      bitrate: 900000
      sample-rate: 96000
      codec: FLAC
      channels: 2
      bit-depth: 24
      fileSize: 12345678
      mtime: 1700000000000000000
  lists:
    - id: 7
      parentId: 0
      name: Favorites
      tracks: [42]
```

List-only payload:

```yaml
version: 1
libraryId: 123e4567-e89b-12d3-a456-426614174000
export_mode: listOnly
library:
  lists:
    - id: 7
      parentId: 0
      name: Favorites
      tracks:
        - uri: music/example.flac
```

## Implementation authority

- [`LibraryYamlExporter.cpp`](../../../../app/runtime/library/LibraryYamlExporter.cpp) defines producer shape and omission rules.
- [`LibraryYamlImporter.cpp`](../../../../app/runtime/library/LibraryYamlImporter.cpp) defines accepted input, validation, normalization, and compatibility behavior.
- [`TrackField.cpp`](../../../../app/runtime/TrackField.cpp) defines canonical metadata and technical field IDs.
- [`AudioCodec.h`](../../../../include/ao/AudioCodec.h) defines codec names and case-insensitive parsing.
- [`RymlAdapter.h`](../../../../include/ao/yaml/RymlAdapter.h) defines scalar conversion and YAML adapter behavior.

## Test authority

- [`LibraryExportImportTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportTest.cpp) covers full/metadata/delta examples, fields, URI normalization, overlays, reports, and previews.
- [`LibraryExportImportListTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportListTest.cpp) covers list-only shape, reference variants, parents, dangling references, and ordering.
- [`LibraryExportImportCoverArtTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportCoverArtTest.cpp) covers ordered covers, aliases, replacement, and removal.
- [`LibraryExportImportErrorTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportErrorTest.cpp) covers rejected structures and scalar values.

## Related documents

- [Library YAML transfer specification](../../../spec/library/runtime/yaml-transfer.md)
- [Reusable YAML adapter specification](../../../spec/persistence/yaml-adapter.md)
- [Library architecture](../../../architecture/library.md)
- [Track model](../model/track.md) for codec, technical-property, and cover behavior
- [Runtime track field catalog](../model/track-field.md) for application field ids and capabilities
- [Supported audio files](../../media/audio-file.md) for imported codec and cover sources
