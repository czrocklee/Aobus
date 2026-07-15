---
id: library.database
type: reference
status: current
domain: library
summary: Defines the version 4 host-local LMDB environment, named databases, keys, records, and validation gates.
---
# Library database

## Scope and version

This reference defines physical library format version `4`, gated by `ao::library::kLibraryVersion`.
It owns the LMDB environment, named databases, key encodings, record composition, size and alignment requirements, and version policy.

Entity meaning belongs to the [track](../model/track.md) and [list](../model/list.md) references.
Scan and identity behavior belongs to the [scan and audio identity specification](../../../spec/library/runtime/scan-and-identity.md).

## Code boundary

This surface belongs to the **core libraries** layer in the [system architecture](../../../architecture/system-overview.md).
`ao::library::MusicLibrary`, its stores, builders, views, and LMDB adapter dependencies live under `include/ao/library/` and `lib/library/`; application-runtime commands may coordinate them but do not own or redefine this format.

## Environment

The library is one LMDB environment at the database path passed to `MusicLibrary::open`; normal application composition supplies `<music-root>/.aobus/library`.
`MusicLibrary::open` uses `MDB_NOTLS`, allows eight named databases, and defaults to a 1 GiB map unless `MusicLibrary::Options::mapSize` overrides it.

The database is a host-local rebuildable index rather than an interchange format.
Integer keys use LMDB native word order and record structs are host-endian; [library YAML](../format/yaml.md) is the portable interchange surface.

## Named databases

| Database | Key | Value |
|---|---|---|
| `meta` | Fixed integer record id | Record `1`: `MetadataHeader`; record `2`: one `std::uint64_t` library revision. |
| `tracks_hot` | `TrackId` integer | `TrackHotHeader`, tag-id array, title bytes. |
| `tracks_cold` | `TrackId` integer | `TrackColdHeader`, optional block payloads, URI bytes. |
| `lists` | `ListId` integer | `ListHeader`, track-id array, name/description/filter bytes. |
| `resources` | Content-derived `ResourceId` integer | Raw blob bytes. |
| `dictionary` | `DictionaryId` integer | Raw UTF-8 bytes without a terminator. |
| `file_manifest` | Root-relative URI padded to a four-byte multiple | `FileManifestHeader`. |

One database slot remains spare.

## Keys and identifiers

All integer identifiers are 32-bit values and reserve `0` as invalid.

- Track and list writers allocate `maxKey + 1`; the first id is `1`, and exhaustion returns `ResourceExhausted`.
- A track is appended to `tracks_hot` first and written to `tracks_cold` under the same id.
- A resource key starts from the low 32 bits of XXH3-64, remaps zero to one, and linearly probes with full-content comparison; identical bytes reuse the existing id.
- The dictionary persists id-to-string rows and rebuilds its string-to-id index in memory when opened.
- Manifest keys are normalized root-relative URI bytes, limited to 500 bytes, then zero-padded to a four-byte multiple; an oversized key returns `ValueTooLarge`.

## Metadata records

`MetadataHeader` is 40 bytes and contains:

| Field | Representation |
|---|---|
| `magic` | `0x42534C52` |
| `libraryVersion` | Unsigned 32-bit format version. |
| `flags` | Unsigned 32-bit flags. |
| `createdTime` | Millisecond system timestamp. |
| `libraryId` | 16 UUID bytes. |

The revision record is an unsigned 64-bit integer.
It is bumped inside each committing library mutation transaction.

## Track records

`TrackHotHeader` is 36 bytes, four-byte aligned, and contains the filter/sort working set: tag bloom, artist/album/genre/album-artist/composer dictionary ids, sample rate, year, title length, tag-blob length, bit depth, and codec.
The tag-id array follows the header, followed by title bytes.

`TrackColdHeader` is 32 bytes, four-byte aligned, and contains duration, bitrate, track/disc numbers and totals, five block offsets, URI offset/length, channels, and one reserved byte.
Defined block slots are written in slot order:

| Slot | Payload |
|---|---|
| `0` cover art | Zero or more eight-byte `CoverArtEntry` values: `ResourceId`, picture type, three reserved bytes. |
| `1` classical | One 24-byte `TrackClassicalBlock`: five dictionary ids plus movement number and total. |
| `2` custom metadata | Eight-byte header, eight-byte entries, then value bytes. |
| `3` and `4` | Reserved and zero. |

An absent payload has offset zero.
Every present payload starts on a four-byte boundary, and URI bytes follow the block area.
The fixed per-track value cost is 68 bytes before arrays, blocks, title, and URI bytes.

## List records

`ListHeader` is 20 bytes and four-byte aligned.
It contains track-id count, offset/length pairs for name, description, and filter, and a parent list id.
The track-id array immediately follows the header; string offsets are relative to the start of that array region.

## Resource and dictionary records

Resource values are raw blob bytes with no header.
Dictionary values are raw UTF-8 bytes with no header or terminator.

## Manifest records

`FileManifestHeader` is 48 bytes and four-byte aligned:

| Field | Representation |
|---|---|
| Track | `TrackId`. |
| File size | Low/high unsigned 32-bit halves of one unsigned 64-bit value. |
| Modification time | Low/high unsigned 32-bit halves of one unsigned 64-bit value. |
| Audio payload length | Low/high unsigned 32-bit halves of one unsigned 64-bit value. |
| Audio signature | 16-byte XXH128 canonical big-endian serialization. |
| Status | `Available = 0`, `Missing = 1`, or `Error = 2`. |
| Padding | Three zero bytes. |

Zero payload length together with an all-zero signature means pending audio identity.

## Validation rules

Builders are the only record producers and own overflow and structural validation before bytes reach a store.
Store writes require record sizes compatible with the layout and four-byte alignment.

Read views perform one constant-time structural gate that proves the fixed header and all derived slices remain inside the record.
A failed gate invalidates the complete record side: validity reports false and accessors return zero or empty values without reading out of bounds.
Semantic in-bounds corruption is reserved for diagnostic deep verification and does not add a per-row scan.

Record validation operates inside the mapped-storage fault-containment limit defined by the [LMDB operation specification](../../../spec/storage/lmdb-operation.md#failure-and-cancellation).
It cannot turn an underlying mapped-file fault into a recoverable record-validation result.

## Compatibility and versioning

Opening a database whose metadata magic or stored library version is invalid returns `CorruptData`.
There is no in-place migration path; reset and rescan rebuild the host-local index.

Version `4` also gates the interpretation of Smart List `filter` text.
The exact accepted surface belongs to the [predicate language reference](../../query/predicate-language.md), and membership meaning belongs to the [predicate evaluation specification](../../../spec/query/predicate-evaluation.md).

Any incompatible key, record, enum encoding, slot meaning, signature-algorithm, or stored Smart List predicate change must increment `kLibraryVersion`.
A predicate change is incompatible when it expands the storable surface beyond what an existing same-version reader accepts, or when it can alter whether existing filter text parses or compiles, what it binds to, or which tracks it matches, even if `ListHeader` and its stored bytes do not change.
An explicitly tested future migration may replace reset-and-rescan recovery for an old version only when it reads the old predicate contract, converts or validates every affected filter atomically, and updates the metadata version after the converted data is valid; the target still has an incremented `kLibraryVersion`, and no such migration exists today.

## Implementation authority

- [`MetadataLayout.h`](../../../../include/ao/library/MetadataLayout.h) owns magic, version, and metadata sizes.
- [`TrackLayout.h`](../../../../include/ao/library/TrackLayout.h), [`ListLayout.h`](../../../../include/ao/library/ListLayout.h), and [`FileManifestLayout.h`](../../../../include/ao/library/FileManifestLayout.h) own binary structs and static size checks.
- [`MusicLibrary.cpp`](../../../../lib/library/MusicLibrary.cpp) owns environment and named-database creation.
- Store and builder implementations under [`lib/library/`](../../../../lib/library/) own key allocation and write validation.

## Test authority

- [`MusicLibraryTest.cpp`](../../../../test/unit/library/MusicLibraryTest.cpp) covers environment, metadata, revision, and version behavior.
- Layout and serialization tests under [`test/unit/library/`](../../../../test/unit/library/) lock sizes, alignment, validation, and store behavior.

## Related documents

- [Resource blob](../../resource/blob.md)

- [Library architecture](../../../architecture/library.md)
- [LMDB operation specification](../../../spec/storage/lmdb-operation.md)
- [Library access and mutation](../../../spec/library/runtime/mutation.md)
- [Library scan and audio identity](../../../spec/library/runtime/scan-and-identity.md)
- [List model](../model/list.md)
- [Predicate language](../../query/predicate-language.md)
- [Persistence and managed-state architecture](../../../architecture/persistence-and-managed-state.md)
