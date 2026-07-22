---
id: library.yaml-format
type: reference
status: current
domain: library
summary: Defines version 2 of the portable, fail-closed YAML library interchange format.
---
# Library YAML format

## Scope and version

This reference defines the exact version 2 YAML surface emitted by `LibraryYamlExporter` and accepted by `LibraryYamlImporter`.
It owns field names, node kinds, scalar widths, accepted values, omission rules, URI syntax, and compatibility behavior.

Transfer modes, restore and merge behavior, authorization, atomicity, reports, and change publication belong to the [library YAML transfer specification](../../../spec/library/runtime/yaml-transfer.md).
CLI commands and output conventions belong to the [CLI command reference](../../cli/command.md).

## Code boundary

This interchange surface belongs to the **application runtime** boundary in the [system architecture](../../../architecture/system-overview.md).
Producer and consumer code lives under `app/runtime/library/`; the format translates core library values but is independent of the host-local `ao::library` storage layout.

## Document root

The root is a closed map with this shape:

```yaml
version: 2
libraryId: 123e4567-e89b-12d3-a456-426614174000
export_mode: full
library:
  tracks: []
  lists: []
```

| Field | Required | Producer | Type and values |
|---|---|---|---|
| `version` | Yes. | Always `2`. | Unsigned 32-bit integer; only `2` is accepted. |
| `libraryId` | No. | Always emitted. | UUID text with hexadecimal digits and hyphens in `8-4-4-4-12` grouping; letter case is ignored. |
| `export_mode` | Yes. | Always emitted. | `delta`, `metadata`, `full`, or `listOnly`. |
| `library` | Yes. | Always emitted. | Closed map containing only `tracks` and `lists`. |

Collection presence declares payload scope:

| `export_mode` | `library.tracks` | `library.lists` |
|---|---|---|
| `delta`, `metadata`, `full` | Required sequence, including when empty. | Required sequence, including when empty. |
| `listOnly` | Forbidden. | Required sequence, including when empty. |

Unknown root or `library` fields reject the complete document.
An absent required collection is not interpreted as an empty collection.

## Library URI

A library URI names one item beneath the music root.
Parsing replaces backslashes with forward slashes and applies lexical normalization.
The result must be non-empty, at most 500 bytes, relative, have no root name or root directory, contain no C0 or DEL control character, contain no surviving `..` component, and never begin with a separator.
POSIX absolute paths, Windows drive paths, UNC paths, and parent traversal are rejected.
Percent signs have no escape semantics: text such as `%2e%2e` names a literal path component and is never decoded into traversal.

The canonical stored and emitted representation uses forward slashes and has no trailing separator.
Manifest operations require callers to supply that canonical representation exactly; they do not silently create a second key for an equivalent spelling.
Every supported file-access boundary resolves the URI against the weakly canonical music root and rejects it if a symlink component resolves outside that root or cannot be resolved because its target is missing.
The music root and an ordinary non-symlink destination suffix may be absent, so a first-run metadata restore can preserve tracks before their audio directory exists.
An existing in-root symlink uses its canonical target identity; a symlink into a different tree is outside the library namespace even when that target contains playable audio.
This is a containment contract, not a hostile-filesystem sandbox: the library tree must not be adversarially replaced between resolution and the operating-system open.

## Track records

Each track is a closed map.

| Field | Required | Type | Producer behavior |
|---|---|---|---|
| `id` | No. | Unsigned 32-bit integer. | Always emitted; nonzero producer-local identity. |
| `uri` | Yes. | Library URI string. | Always emitted. |
| Metadata fields | No. | String or unsigned 16-bit integer according to the tables below. | Mode-dependent. |
| `custom` | No. | Map of arbitrary scalar string keys to scalar string values. | Emitted only when non-empty. |
| `tags` | No. | Sequence of scalar strings. | Emitted only when non-empty. |
| `covers` | No. | Sequence of closed cover maps. | Mode-dependent; an empty sequence is meaningful. |
| Technical fields | No. | Scalars from the technical table. | Emitted in `full`. |
| `fileSize` | No. | Unsigned 64-bit integer. | Emitted in `full`; `0` when no manifest row exists. |
| `mtime` | No. | Unsigned 64-bit integer. | Emitted in `full`; `0` when no manifest row exists. |

Track `id` values need not match target-library IDs.
Duplicate nonzero IDs reject the document; `0` and omitted IDs create no ID mapping for list references.
Duplicate canonical track URIs also reject the document, including records whose input spellings normalize to the same URI.
Keys in one `custom` map must be unique.

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

These names come from `rt::trackFieldId()` and use hyphens rather than underscores or camel case.

### Technical properties

| Field | Type | Units or accepted values |
|---|---|---|
| `duration` | Unsigned 32-bit integer. | Milliseconds. |
| `bitrate` | Unsigned 32-bit integer. | Bits per second. |
| `sample-rate` | Unsigned 32-bit integer. | Hertz. |
| `codec` | String. | Case-insensitive `UNKNOWN`, `FLAC`, `ALAC`, `WAV`, `AAC`, or `MP3`. |
| `channels` | Unsigned 8-bit integer. | Channel count. |
| `bit-depth` | Unsigned 8-bit integer. | Bits per sample. |

Any other codec token rejects the complete document.

### Cover records

`covers` is an ordered sequence of closed maps:

```yaml
covers:
  - type: 3
    data: iVBORw0KGgo=
```

| Field | Required | Type |
|---|---|---|
| `type` | Yes. | Unsigned 32-bit integer from `0` through `20`, matching the APIC/FLAC picture-type vocabulary. |
| `data` | Yes. | Non-empty base64 scalar. |

Unknown fields, out-of-range types, malformed base64, and empty decoded data reject the complete document.
The producer may attach YAML anchors to first occurrences of shared image data and aliases to later occurrences; the importer resolves those aliases before validation.
If a stored cover references a missing or empty core resource, export fails with `CorruptData` instead of emitting a cover map that the importer would reject.

## List records

Each list is a closed map.

| Field | Required | Type and meaning |
|---|---|---|
| `id` | Yes. | Nonzero unique unsigned 32-bit payload identity. |
| `parentId` | No. | Unsigned 32-bit payload list identity; omission or `0` means root. |
| `name` | Yes. | Scalar string. |
| `description` | No. | Scalar string. |
| `filter` | No. | Scalar predicate text; presence selects a Smart List. |
| `tracks` | No. | Sequence of manual-list track references. |

`filter` and `tracks` are mutually exclusive.
A present `filter` must be non-empty and must parse and compile under the current query grammar.
Omitting both creates an empty manual list.
The producer always emits `id`, `parentId`, and `name`; it omits empty descriptions and empty manual membership.

Manual-list track references accept these forms:

```yaml
tracks:
  - 42
  - id: 42
  - uri: music/example.flac
```

A scalar or `id` map refers to a track record's payload `id`.
A `uri` map contains a Library URI and resolves through the target manifest.
A map must contain exactly one of `id` or `uri`; unknown fields and ambiguous maps reject the document.
Normal exports use scalar IDs, while `listOnly` exports use URI maps so lists can attach to tracks with different target IDs.

Dangling parent, track-ID, and URI references are ignored and counted in the import report.
Known parent relationships must not point to self or form a cycle.
Duplicate manual-list references collapse to their first resolved occurrence while preserving first-occurrence order.

The core list header uses 16-bit string lengths and offsets.
Each name, description, and filter is therefore limited to 65,535 bytes; the resolved track-ID array is limited to 16,383 entries; and the track-ID bytes plus name and description must fit a 65,535-byte offset.
Exceeding any bound rejects the payload rather than truncating it.

## Validation rules

The importer reports `FormatRejected` for malformed YAML and any violation of this reference, including:

- a non-map root, `library`, track, cover, list, or map-form list reference;
- a missing required field or collection, or a forbidden `tracks` collection in `listOnly`;
- an unsupported version, mode, codec, or cover type;
- an unknown or duplicate field in any closed map;
- a malformed UUID, Library URI, scalar, sequence, or numeric width;
- duplicate nonzero track IDs, duplicate canonical track URIs, duplicate custom keys, or missing, zero, or duplicate list IDs;
- simultaneous `filter` and `tracks`, an empty or invalid filter, a known parent cycle, or an ambiguous list-reference map;
- a URI or list representation exceeding its core storage limit;
- malformed or empty cover data.

The URI and fixed-width list limits above are the format's current explicit resource ceilings.
Version 2 does not otherwise cap total document bytes, aggregate decoded cover bytes, or one decoded cover blob.
No broader transfer budget is currently defined.
The observable failure and rollback contract is defined by the [transfer specification](../../../spec/library/runtime/yaml-transfer.md#failure-and-cancellation).

## Compatibility and versioning

The importer accepts version 2 only.
It has no version-1 reader, legacy mode alias, permissive unknown-field path, restore bypass, or conversion command.
There is no migration contract for earlier interchange files.

Changing a field name, node kind, scalar width, accepted enum value, omission meaning, or predicate interpretation requires a new format version unless the change only narrows producer output within this accepted version-2 surface.
Payload versioning is independent of the host-local database's `kLibraryVersion`.

## Examples

Full payload:

```yaml
version: 2
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
version: 2
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
- [`LibraryYamlImporter.cpp`](../../../../app/runtime/library/LibraryYamlImporter.cpp) defines accepted input and validation.
- [`LibraryUri`](../../../../include/ao/library/LibraryUri.h) defines the path namespace.
- [`TrackField.cpp`](../../../../app/runtime/TrackField.cpp) defines canonical metadata and technical field IDs.
- [`AudioCodec.h`](../../../../include/ao/AudioCodec.h) defines codec names and case-insensitive parsing.

## Test authority

- [`LibraryExportImportTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportTest.cpp) covers modes, fields, URI normalization, overlays, reports, and previews.
- [`LibraryExportImportListTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportListTest.cpp) covers list-only shape, references, parents, dangling references, and ordering.
- [`LibraryExportImportCoverArtTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportCoverArtTest.cpp) covers ordered covers, aliases, replacement, and removal.
- [`LibraryYamlSchemaTest.cpp`](../../../../test/unit/runtime/library/LibraryYamlSchemaTest.cpp) covers closed-schema, scope, enum, and URI rejection.
- [`LibraryExportImportErrorTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportErrorTest.cpp) covers scalar validation and transactional rollback.
- [`LibraryUriTest.cpp`](../../../../test/unit/library/LibraryUriTest.cpp) covers canonicalization, literal percent text, control-character rejection, absent roots, in-root resolution, and escaping or dangling symlinks.

## Related documents

- [Library YAML transfer specification](../../../spec/library/runtime/yaml-transfer.md)
- [Reusable YAML adapter specification](../../../spec/persistence/yaml-adapter.md)
- [Library architecture](../../../architecture/library.md)
- [Predicate language](../../query/predicate-language.md)
- [Track model](../model/track.md)
