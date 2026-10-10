---
id: library.yaml-format
---
# Library YAML format

## Scope and version

This reference defines the exact version 7 YAML surface emitted by `LibraryYamlExporter` and accepted by `LibraryYamlImporter`.
It owns field names, node kinds, scalar widths, accepted values, omission rules, URI syntax, and compatibility behavior.

Transfer modes, restore and merge behavior, authorization, atomicity, reports, and change publication belong to the [library YAML transfer specification](../../../system/library/yaml-transfer.md).
CLI commands and output conventions belong to the [CLI command reference](../../cli/command.md).

## Code boundary

This interchange surface belongs to the **application runtime** boundary in the [system architecture](../../../system/overview.md).
Producer and consumer code lives under `app/runtime/library/`; the format translates core library values but is independent of the host-local `ao::library` storage layout.

## Document root

The root is a closed map with this shape:

```yaml
version: 7
libraryId: 123e4567-e89b-12d3-a456-426614174000
export_mode: full
library:
  resources: []
  tracks: []
  lists: []
```

| Field | Required | Producer | Type and values |
|---|---|---|---|
| `version` | Yes. | Always `7`. | Unsigned 32-bit integer; only `7` is accepted. |
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
Unicode text normalization is not part of URI canonicalization: path bytes retain filesystem identity across transfer.
Manifest operations require callers to supply that canonical representation exactly; they do not silently create a second key for an equivalent spelling.
Every supported file-access boundary resolves the URI against the weakly canonical music root and rejects it if a symlink component resolves outside that root or cannot be resolved because its target is missing.
The music root and an ordinary non-symlink destination suffix may be absent, so a first-run metadata restore can preserve tracks before their audio directory exists.
An existing in-root symlink uses its canonical target identity; a symlink into a different tree is outside the library namespace even when that target contains playable audio.
This is a containment contract, not a hostile-filesystem sandbox: the library tree must not be adversarially replaced between resolution and the operating-system open.

## Library text scalars

Track metadata, tag names, custom-metadata keys and values, and List name, description, and filter values must be scalar-valid UTF-8.
The importer normalizes Track text and List name and description to NFC at the core library admission boundary; the exporter emits those fields in NFC from a current physical library.
List filter source remains byte-exact after UTF-8 validation because its URI literals carry filesystem identity.
Malformed sequences reject the document and are never repaired with U+FFFD.
Size limits apply to the normalized NFC bytes of admitted display text and to the original bytes of a List filter.

Custom-metadata keys must remain unique after NFC normalization.
For example, precomposed `é` and an `e` followed by a combining acute accent are the same key for this rule.
Tags are set members, so canonically equivalent tag spellings collapse to one member through dictionary identity.
These rules do not case-fold display text and do not apply Unicode normalization to Library URIs or opaque List filter source.

## Track records

Each track is a closed map.

| Field | Required | Type | Producer behavior |
|---|---|---|---|
| `id` | No. | Unsigned 32-bit integer. | Always emitted; nonzero producer-local identity. |
| `uri` | Yes. | Library URI string. | Always emitted. |
| Metadata fields | No. | String or unsigned 16-bit integer according to the tables below. | Mode-dependent. |
| `recording-date` | No. | Canonical partial-date scalar, or empty scalar text that clears the value. | Mode-dependent; see [recording date](#recording-date). |
| `credits` | No. | Sequence of closed `name`/`kind`/`role` maps. | Explicit sequence, including empty, in full/metadata; difference from the selected baseline in delta. See [Credits](#credits). |
| `custom` | No. | Map of arbitrary scalar string keys to scalar string values. | Emitted only when non-empty. |
| `tags` | No. | Sequence of scalar strings. | Emitted only when non-empty. |
| `covers` | No. | Sequence of closed cover maps. | Mode-dependent; an empty sequence is meaningful. |
| Technical fields | No. | Scalars from the technical table. | Emitted in `full`. |
| `fileSize` | No. | Unsigned 64-bit integer. | Emitted in `full`; `0` when no manifest row exists. |
| `mtime` | No. | Closed `seconds`/`nanoseconds` map or an explicit YAML null. | Emitted in `full`: the map when the manifest stores a modification time, an explicit null when it stores none, including when no manifest row exists. |

`mtime` carries the manifest's modification time as one portable instant on the Unix/POSIX time scale: `seconds` is a signed 64-bit count from `1970-01-01 00:00:00 UTC`, excluding leap seconds, and `nanoseconds` is the fraction of that second, at least `0` and below `1000000000`.
The map is closed and contains exactly both keys; an explicit YAML null records that no modification time is known, while the epoch instant `seconds: 0` with `nanoseconds: 0` is a present value distinct from it.
The value is one defined instant with no UTC-versus-local meaning and no host file-clock interpretation; timezone is a display concern only.
Transfer carries the full seconds-and-nanoseconds precision verbatim.

Track `id` values need not match target-library IDs.
Duplicate nonzero IDs reject the document; `0` and omitted IDs create no ID mapping for list references.
Duplicate canonical track URIs also reject the document, including records whose input spellings normalize to the same URI.
A track record must name a supported audio file from the [supported audio files reference](../../media/audio-file.md): a URI with any other extension rejects the document, matching the admission rule of manual track creation.
Keys in one `custom` map must be unique both as YAML keys and after NFC normalization.

### Text metadata

The following fields are scalar strings:

| Field | Field | Field |
|---|---|---|
| `title` | `artist` | `album` |
| `album-artist` | `genre` | `composer` |
| `work` | `movement` | |

### Numeric metadata

The following fields are unsigned 16-bit integers:

| Field | Field | Field |
|---|---|---|
| `year` | `track-number` | `track-total` |
| `disc-number` | `disc-total` | `movement-number` |
| `movement-total` | | |

These names come from `rt::trackFieldId()` and use hyphens rather than underscores or camel case.

### Recording date

`recording-date` is a scalar string holding one partial date on the proleptic Gregorian calendar, written canonically as `YYYY`, `YYYY-MM`, or `YYYY-MM-DD` with zero-padded components and years `0001` through `9999`.
The producer emits the stored precision exactly; an import stores that same precision and never invents a missing month or day.
The importer does not trim the scalar text, and editors trim or clear their input before producing a document.
The clear form is empty scalar text. A quoted empty scalar (`''` or `""`) and a present empty plain scalar (`recording-date:`) both record the absence sentinel and clear a stored date.
RapidYAML marks that empty plain scalar `VALNIL`; the importer reads the scalar text and does not treat the flag as a separate null form.
The literal texts `null` and `~` are nonempty, so they are not the clear form and fail the canonical-date grammar. This field has no separate null acceptance or null-rejection rule, unlike `mtime`.
Whitespace that remains in the scalar text is rejected. An unquoted plain scalar may already have had surrounding whitespace removed by the YAML lexer, so that form does not carry whitespace into this check.
An omitted key preserves the baseline value under the mode-specific overlay rules, like any other scalar field.

The importer validates the complete scalar in the document preflight: any other shape, an impossible calendar date such as `1981-02-30`, or year `0000` rejects the complete document with `FormatRejected` before any durable effect.
The value describes when a performance was recorded and is independent of the scalar `year`; it is never inferred from release metadata, and no media import populates it, so a `delta` file baseline always carries an absent date.

### Credits

`credits` is a sequence of closed maps, one entry per credit:

```yaml
credits:
  - name: Example Orchestra
    kind: ensemble
  - name: Anne Example
    kind: soloist
    role: violin
```

`name` and `kind` are required scalars; `role` is an optional scalar.
Kind tokens are exactly `conductor`, `ensemble`, `soloist`, and `performer`.
The importer groups entries stably in that canonical kind order, and the exporter emits that order.
Within-kind order, duplicate entries, repeated names and roles, and the same name in different kinds are preserved; there is no deduplication.
The separate `conductor`, `ensemble`, `soloist`, and `musicians` track keys are unknown fields, not aliases.

An omitted role, `role:`, quoted empty role, or ASCII-blank role means absence; the exporter omits absent roles.
Literal `null` and `~` role text is nonempty descriptive text and is preserved, not treated as absence.
Names and roles have only the six ASCII whitespace characters (space, tab, carriage return, line feed, form feed, vertical tab) trimmed at their boundaries, followed by scalar UTF-8 validation and NFC normalization.
A no-break space (U+00A0) is not trimmed and does not make a name blank.
Unknown or duplicate keys, unknown kind tokens, wrong node kinds, malformed text, and blank names reject the complete document in preflight with `FormatRejected` before any durable effect.

A present sequence replaces all credits; `credits: []` clears all.
Null, empty scalar, or map values in place of the sequence are rejected.
Omission preserves the selected baseline: stored credits on merge into an existing track, media credits on delta creation/restore when readable (otherwise empty), and empty metadata on metadata/full creation or restore.
Full and metadata exports carry the sequence explicitly even when empty. Delta exports carry it only when complete canonical credits differ from the selected media or empty fallback baseline, including an explicit clear.
List-only payloads exclude track metadata.
The runtime-owned `credits` key and every exact [runtime TrackField id](../../../../app/include/ao/rt/TrackField.h) are reserved custom-metadata keys, including technical and display fields.
Matching is case-sensitive after NFC normalization; query aliases and camel-case spellings are not aliases here.
For example, `credits` and `sample-rate` are reserved, while `Credits`, `credit`, `albumArtist`, and `musicians` remain ordinary custom keys.

### Technical properties

The `duration` scalar remains an unsigned 32-bit millisecond value.
Library storage uses signed 32-bit `TrackDuration`, so import rejects values above `2147483647` ms with `FormatRejected` rather than narrowing or clamping them.
This is a core-storage representability check, not a change to the scalar grammar or database layout; `0` (unknown) and `2147483647` ms round-trip exactly.

| Field | Type | Units or accepted values |
|---|---|---|
| `duration` | Unsigned 32-bit integer. | Milliseconds. |
| `bitrate` | Unsigned 32-bit integer. | Bits per second. |
| `sample-rate` | Unsigned 32-bit integer. | Hertz. |
| `codec` | String. | Case-insensitive `UNKNOWN`, `FLAC`, `ALAC`, `WAV`, `AAC`, `MP3`, or `Opus`. |
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
- an `mtime` value that is neither an explicit YAML null nor a closed `seconds`/`nanoseconds` map, a map missing either key or carrying an extra key, a `seconds` value outside the signed 64-bit range, or a `nanoseconds` value at or above `1000000000`;
- a `recording-date` scalar whose text is neither empty nor a canonical `YYYY`, `YYYY-MM`, or `YYYY-MM-DD` date naming a real calendar day, including the nonempty texts `null` and `~` and any whitespace that remains in the scalar;
- a `credits` node that is not a sequence of closed maps with required scalar `name` and `kind` and optional scalar `role`, an unknown kind token, or a name or role the shared Credits admission gate rejects;
- a track record whose URI is not a supported audio file;
- malformed UTF-8 in library text or a normalized text value beyond its core storage limit;
- duplicate nonzero track IDs, duplicate canonical track URIs, raw or canonically equivalent duplicate custom keys, or missing, zero, or duplicate list IDs;
- an invalid non-empty filter, a known parent cycle, or an ambiguous list-order reference map;
- a URI or list representation exceeding its core storage limit;
- a malformed digest, an out-of-range `length`, a cover naming no row, a row no track references, or two rows carrying one digest.

The recording date and the complete Credits list are validated in the document preflight, so their violations reject the whole document before any durable import effect.
The URI and fixed-width list limits above are the format's current explicit resource ceilings.
Version 7 does not otherwise cap total document bytes; covers contribute a fixed-size row each rather than their content.
The observable failure and rollback contract is defined by the [transfer specification](../../../system/library/yaml-transfer.md#failure-and-cancellation).

## Compatibility and versioning

The importer accepts version 7 only.
It has no reader for versions 1 through 6, legacy `tracks` List field, permissive unknown-field path, restore bypass, or conversion command.
A version-6 document is rejected with `FormatRejected` rather than converted.
There is no migration contract for earlier interchange files, and a version-3 document's embedded cover bytes cannot be read by this version.

Changing a released field name, node kind, scalar width, accepted enum value, omission meaning, predicate interpretation, or rank-reference interpretation requires a new format version unless the change only narrows producer output within the accepted surface.
Version 7 accepts only the schema defined here, including `recording-date` and typed `credits`; alternative layouts are unsupported even when labeled version 7. There is no dual reader or layout autodetection.
`recording-date` defines empty scalar text, including quoted empty scalars and a present empty plain scalar, as its clear form.
Version 6 existed for one such change: `mtime` stopped being an unsigned host file-clock scalar and became the portable `seconds`/`nanoseconds` instant map or an explicit YAML null.
Version 5 recorded the Unicode-caseless meaning of `~`, which later versions carry forward; decomposed scalar-valid display text remains accepted and is emitted in the canonical representation required by physical database version 9.
Payload versioning is independent of the host-local database's `kLibraryVersion`.

## Examples

Full payload:

```yaml
version: 7
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
      recording-date: 1981-05-12
      credits:
        - name: Keith
          kind: performer
          role: Guitar
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
      mtime:
        seconds: 1700000000
        nanoseconds: 123456789
  lists:
    - id: 7
      parentId: 0
      name: Favorites
      filter: "#favorite"
      order: [42]
```

List-only payload:

```yaml
version: 7
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
- [`RecordingDate.h`](../../../../include/ao/library/RecordingDate.h) defines the partial-date value, its canonical text forms, and the parse behind `recording-date`.
- [`Credits.h`](../../../../include/ao/library/Credits.h) defines the shared kind/name/role admission gate behind `credits`.
- [`FileTimestamp.h`](../../../../include/ao/FileTimestamp.h) defines the portable modification-time instant carried by `mtime`.
- [`TrackField.cpp`](../../../../app/runtime/TrackField.cpp) defines canonical metadata and technical field IDs.
- [`AudioCodec.h`](../../../../include/ao/AudioCodec.h) defines codec values and their stable storage representation.
- [`AudioCodecText.h`](../../../../include/ao/AudioCodecText.h) defines codec display names and case-insensitive parsing.

## Test authority

- [`LibraryExportImportTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportTest.cpp) covers modes, fields, URI normalization, overlays, reports, and previews.
- [`LibraryExportImportListTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportListTest.cpp) covers list-only shape, references, parents, dangling references, and ordering.
- [`LibraryExportImportCoverArtTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportCoverArtTest.cpp) covers the resource table's shape, ordering, determinism, closure rules, digest and length rejection, and each mode's cover terminal state.
- [`LibraryYamlSchemaTest.cpp`](../../../../test/unit/runtime/library/LibraryYamlSchemaTest.cpp) covers closed-schema, scope, enum, and URI rejection.
- [`LibraryExportImportErrorTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportErrorTest.cpp) covers scalar validation, duration boundary round trips, and transactional rollback for unrepresentable durations.
- [`LibraryYamlImporterTest.cpp`](../../../../test/unit/runtime/library/LibraryYamlImporterTest.cpp) covers recording-date and credit-entry admission, whole-document preflight rejection, and version gating before field interpretation.
- [`LibraryYamlExporterTest.cpp`](../../../../test/unit/runtime/library/LibraryYamlExporterTest.cpp) covers recording-date and credits emission, omission, and delta baselines.
- [`LibraryYamlPerformanceMetadataTest.cpp`](../../../../test/unit/runtime/library/LibraryYamlPerformanceMetadataTest.cpp) covers same-version round trips of date precision, list order and roles, clear forms, and merge overlay preservation.
- [`LibraryUriTest.cpp`](../../../../test/unit/library/LibraryUriTest.cpp) covers canonicalization, literal percent text, control-character rejection, absent roots, in-root resolution, and escaping or dangling symlinks.

## Related documents

- [Library YAML transfer specification](../../../system/library/yaml-transfer.md)
- [Reusable YAML adapter specification](../../../system/persistence/yaml-adapter.md)
- [Library architecture](../../../system/library/structure.md)
- [Predicate language](../../query/predicate-language.md)
- [Track model](../model/track.md)
