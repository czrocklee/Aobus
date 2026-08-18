---
id: library.yaml-format
type: reference
status: current
domain: library
summary: Defines version 4 of the portable, fail-closed YAML library interchange format.
---
# Library YAML format

## Scope and version

This reference defines the exact version 4 YAML surface emitted by `LibraryYamlExporter` and accepted by `LibraryYamlImporter`.
It owns field names, node kinds, scalar widths, accepted values, omission rules, URI syntax, and compatibility behavior.

Transfer modes, restore and merge behavior, authorization, atomicity, reports, and change publication belong to the [library YAML transfer specification](../../../spec/library/runtime/yaml-transfer.md).
CLI commands and output conventions belong to the [CLI command reference](../../cli/command.md).

## Code boundary

This interchange surface belongs to the **application runtime** boundary in the [system architecture](../../../architecture/system-overview.md).
Producer and consumer code lives under `app/runtime/library/`; the format translates core library values but is independent of the host-local `ao::library` storage layout.

## Document root

The root is a closed map with this shape:

```yaml
version: 4
libraryId: 123e4567-e89b-12d3-a456-426614174000
export_mode: full
library:
  resources: []
  tracks: []
  lists: []
```

| Field | Required | Producer | Type and values |
|---|---|---|---|
| `version` | Yes. | Always `4`. | Unsigned 32-bit integer; only `4` is accepted. |
| `libraryId` | No. | Always emitted. | UUID text with hexadecimal digits and hyphens in `8-4-4-4-12` grouping; letter case is ignored. |
| `export_mode` | Yes. | Always emitted. | `delta`, `metadata`, `full`, or `listOnly`. |
| `library` | Yes. | Always emitted. | Closed map containing only `resources`, `tracks`, and `lists`. |

Collection presence declares payload scope:

| `export_mode` | `library.resources` | `library.tracks` | `library.lists` |
|---|---|---|---|
| `full` | Required sequence, including when empty. | Required sequence, including when empty. | Required sequence, including when empty. |
| `delta`, `metadata` | Forbidden. | Required sequence, including when empty. | Required sequence, including when empty. |
| `listOnly` | Forbidden. | Forbidden. | Required sequence, including when empty. |

No document of any mode carries a cover byte.

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

### Resource records

`library.resources` is the document's table of cover content, emitted in `full` only.
Each row is a closed map, and rows appear in ascending digest order, so two exports of one unchanged library are byte-identical:

```yaml
resources:
  - digest: 3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b
    length: 174829
```

| Field | Required | Type |
|---|---|---|
| `digest` | Yes. | Exactly 64 lowercase hexadecimal characters: the SHA-256 digest of the cover content. |
| `length` | Yes. | Unsigned 32-bit integer: the length of that content. |

A `full` export emits exactly the descriptors its exported tracks reference, not every row the database holds.

### Cover records

`covers` is an ordered sequence of closed maps, emitted in `full` only:

```yaml
covers:
  - type: 3
    resource: 3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b
```

| Field | Required | Type |
|---|---|---|
| `type` | Yes. | Unsigned 32-bit integer from `0` through `20`, matching the APIC/FLAC picture-type vocabulary. |
| `resource` | Yes. | A digest present in `library.resources`; never a `ResourceId`, which is local to the library that minted it. |

The reference graph must close in both directions: a cover naming no row, a row no track references, and two rows carrying one digest each reject the complete document.
Unknown fields inside a resource or cover map, out-of-range picture types, an uppercase digest spelling, a digest that is not exactly 64 lowercase hexadecimal characters, and a `length` that is negative, non-integral, or above `UINT32_MAX` also reject it.
A `covers` key outside `full` rejects the document, because a mode that carries no table cannot express a reference.

If a stored cover references a missing core descriptor, export fails rather than emitting a document the importer would reject.

## List records

Each list is a closed map.

| Field | Required | Type and meaning |
|---|---|---|
| `id` | Yes. | Nonzero unique unsigned 32-bit payload identity. |
| `parentId` | No. | Unsigned 32-bit payload list identity; omission or `0` means root. |
| `name` | Yes. | Scalar string. |
| `description` | No. | Scalar string. |
| `filter` | No. | Scalar local predicate text; empty or omitted means identity (`true`). |
| `order` | No. | Sequence of saved rank references; empty or omitted means no saved rank. |

`filter` and `order` are independent and may coexist.
A non-empty `filter` must parse and compile under the current query grammar.
The producer always emits `id`, `parentId`, and `name`; it omits an empty description, identity filter, and empty order.
Order references do not define membership and need not currently match the List expression.

Order references accept these forms:

```yaml
order:
  - 42
  - id: 42
  - uri: music/example.flac
```

A scalar or `id` map refers to a track record's payload `id`.
A `uri` map contains a Library URI and resolves through the target manifest.
A map must contain exactly one of `id` or `uri`; unknown fields and ambiguous maps reject the document.
Normal exports use scalar IDs, while `listOnly` exports use URI maps so ranks can attach to tracks with different target IDs.

Dangling parent, track-ID, and URI references are ignored and counted in the import report.
Known parent relationships must not point to self or form a cycle.
Duplicate resolved order references collapse to their first occurrence while preserving first-occurrence order.

Each name, description, and filter is limited to 65,535 bytes by the core product limit.
The resolved order count must fit unsigned 32-bit, and its four-byte entries plus text and canonical padding must fit checked host-size and storage limits.
Exceeding any bound rejects the payload rather than narrowing or truncating it.

## Validation rules

The importer reports `FormatRejected` for malformed YAML and any violation of this reference, including:

- a non-map root, `library`, resource, track, cover, list, or map-form list reference;
- a missing required field or collection, a forbidden `tracks` collection in `listOnly`, or a `resources` table or `covers` key outside `full`;
- an unsupported version, mode, codec, or cover type;
- an unknown or duplicate field in any closed map;
- a malformed UUID, Library URI, scalar, sequence, or numeric width;
- duplicate nonzero track IDs, duplicate canonical track URIs, duplicate custom keys, or missing, zero, or duplicate list IDs;
- an invalid non-empty filter, a known parent cycle, or an ambiguous list-order reference map;
- a URI or list representation exceeding its core storage limit;
- a malformed digest, an out-of-range `length`, a cover naming no row, a row no track references, or two rows carrying one digest.

The URI and fixed-width list limits above are the format's current explicit resource ceilings.
Version 4 does not otherwise cap total document bytes; covers contribute a fixed-size row each rather than their content.
The observable failure and rollback contract is defined by the [transfer specification](../../../spec/library/runtime/yaml-transfer.md#failure-and-cancellation).

## Compatibility and versioning

The importer accepts version 4 only.
It has no reader for versions 1 through 3, legacy `tracks` List field, permissive unknown-field path, restore bypass, or conversion command.
There is no migration contract for earlier interchange files, and a version-3 document's embedded cover bytes cannot be read by this version.

Changing a field name, node kind, scalar width, accepted enum value, omission meaning, predicate interpretation, or rank-reference interpretation requires a new format version unless the change only narrows producer output within this accepted version-4 surface.
Payload versioning is independent of the host-local database's `kLibraryVersion`.

## Examples

Full payload:

```yaml
version: 4
libraryId: 123e4567-e89b-12d3-a456-426614174000
export_mode: full
library:
  resources:
    - digest: 3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b
      length: 174829
  tracks:
    - id: 42
      uri: music/example.flac
      title: Example
      album-artist: Ensemble
      track-number: 1
      tags: [favorite]
      custom:
        mood: focused
      covers:
        - type: 3
          resource: 3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b
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
      filter: "#favorite"
      order: [42]
```

List-only payload:

```yaml
version: 4
libraryId: 123e4567-e89b-12d3-a456-426614174000
export_mode: listOnly
library:
  lists:
    - id: 7
      parentId: 0
      name: Favorites
      filter: "#favorite"
      order:
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
- [`LibraryExportImportCoverArtTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportCoverArtTest.cpp) covers the resource table's shape, ordering, determinism, closure rules, digest and length rejection, and each mode's cover terminal state.
- [`LibraryYamlSchemaTest.cpp`](../../../../test/unit/runtime/library/LibraryYamlSchemaTest.cpp) covers closed-schema, scope, enum, and URI rejection.
- [`LibraryExportImportErrorTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportErrorTest.cpp) covers scalar validation and transactional rollback.
- [`LibraryUriTest.cpp`](../../../../test/unit/library/LibraryUriTest.cpp) covers canonicalization, literal percent text, control-character rejection, absent roots, in-root resolution, and escaping or dangling symlinks.

## Related documents

- [Library YAML transfer specification](../../../spec/library/runtime/yaml-transfer.md)
- [Reusable YAML adapter specification](../../../spec/persistence/yaml-adapter.md)
- [Library architecture](../../../architecture/library.md)
- [Predicate language](../../query/predicate-language.md)
- [Track model](../model/track.md)
