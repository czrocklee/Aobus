---
id: library.database
type: reference
status: current
domain: library
summary: Defines the version 5 host-local LMDB environment, named databases, keys, records, and validation gates.
---
# Library database

## Scope and version

This reference defines physical library format version `5`, gated by `ao::library::kLibraryVersion`.
It owns the LMDB environment, named databases, key encodings, record composition, size and alignment requirements, and version policy.

Entity meaning belongs to the [track](../model/track.md) and [list](../model/list.md) references.
Scan and identity behavior belongs to the [scan and audio identity specification](../../../spec/library/runtime/scan-and-identity.md).

## Code boundary

This surface belongs to the **core libraries** layer in the [system architecture](../../../architecture/system-overview.md).
`ao::library::MusicLibrary`, its stores, builders, views, and LMDB adapter dependencies live under `include/ao/library/` and `lib/library/`; application-runtime commands may coordinate them but do not own or redefine this format.

## Environment

The library is one LMDB environment at the database path passed to `MusicLibrary::open`; normal application composition supplies `<music-root>/.aobus/library`.
`MusicLibrary::open` uses `MDB_NOTLS`, allows eight named databases, and defaults to a 1 GiB map unless `MusicLibrary::Options::mapSize` overrides it.

The database is host-local rather than an interchange format.
It combines regenerable scan facts with user-authored lists, membership, curated metadata, tags, covers, custom metadata, and stable library/track identities; the complete environment is therefore not rebuildable from media files.
Its path must reside on a filesystem local to the host. Network filesystems and
shares are unsupported because neither the writer lease nor LMDB's mapped-file
locking provides cross-host safety for this database.
Integer keys use LMDB native word order and record structs are host-endian; [library YAML](../format/yaml.md) is the portable interchange surface.

## Transaction access

`MusicLibrary::readTransaction()` returns a move-only `ReadTransaction` that directly owns one native LMDB read snapshot.
Failure to begin the native transaction raises the library's general storage exception; this API has no recoverable typed-error channel.
`WritableMusicLibrary::acquire(MusicLibrary&)` returns the explicit move-only capability that can create a `WriteTransaction`; `MusicLibrary` exposes no public write-transaction factory.
The write wrapper owns one native transaction, its transaction-local dictionary writer, the process writer gate, and a shared anchor to the writable capability's lease.

The specialized stores are const service handles.
Their readers accept either library transaction type, while their writers require a mutable `WriteTransaction`.
Native LMDB transaction handles remain private implementation details of `MusicLibrary` and the stores; the wrappers add semantic capability boundaries but no additional storage transaction or heap allocation on the read path.

Writable-capability acquisition non-blockingly locks `<database-path>/.aobus-writer.lock` for the capability lifetime.
An active write transaction retains the lock after its originating capability is destroyed and releases it on commit, failure, abort-by-destruction, or transaction destruction.
The lock file has no governed payload and is not part of format version `5`, but it must not be removed while a writable process is active.

## Named databases

| Database | Key | Value |
|---|---|---|
| `meta` | Fixed integer record id | Record `1`: `MetadataHeader`; record `2`: one `std::uint64_t` library revision. |
| `tracks_hot` | `TrackId` integer | `TrackHotHeader`, tag-id array, title bytes. |
| `tracks_cold` | `TrackId` integer | `TrackColdHeader`, optional block payloads, URI bytes. |
| `lists` | `ListId` integer | `ListHeader`, order-track-id array, name/description/filter bytes, zero padding. |
| `resources` | Content-derived `ResourceId` integer | Raw blob bytes. |
| `dictionary` | `DictionaryId` integer | Raw UTF-8 bytes without a terminator. |
| `file_manifest` | Root-relative URI padded to a four-byte multiple | `FileManifestHeader`. |

One database slot remains spare.

## Keys and identifiers

All integer identifiers are 32-bit values and reserve `0` as invalid.

- Track and list writers allocate `maxKey + 1`; the first id is `1`, and exhaustion returns `ResourceExhausted`.
- A track is appended to `tracks_hot` first and written to `tracks_cold` under the same id.
- A resource key starts from the low 32 bits of XXH3-64, remaps zero to one, and linearly probes with full-content comparison; identical bytes reuse the existing id.
- Dictionary ids produced by current writers are the dense committed range `1..entryCount`; new text receives the next id, repeated text reuses its existing id, and committed ids are never deleted or rebound.
- An aborted transaction-local dictionary tail has no durable identity and its ids may be reused by a later transaction.
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
Its five unsigned 32-bit fields have these stable byte offsets:

| Offset | Field | Meaning |
|---:|---|---|
| `0` | `orderTrackIdCount` | Number of four-byte `TrackId` values in the stored rank overlay. |
| `4` | `nameLength` | Name byte length. |
| `8` | `descLength` | Description byte length. |
| `12` | `filterLength` | Local expression byte length. |
| `16` | `parentId` | Parent `ListId`; zero means the All Tracks root. |

The complete value uses one canonical packing.
This diagram is part of the layout reference and mirrors the authoritative diagram in `ListLayout.h`:

```text
record begin
┌─────────────────────────────────────┐
│        ListHeader (20B)             │
│  orderTrackIdCount (4B)             │
│  nameLength (4B)                    │
│  descLength (4B)                    │
│  filterLength (4B)                  │
│  parentId (4B)                      │
├─────────────────────────────────────┤  variable region begin
│  order track ID 1 (4B)              │
│  order track ID 2 (4B)              │
│  ...                                │
├─────────────────────────────────────┤  derived name offset
│  name string...                     │
├─────────────────────────────────────┤  derived description offset
│  description string...              │
├─────────────────────────────────────┤  derived filter offset
│  filter expression string...        │
├─────────────────────────────────────┤
│  zero padding (0..3B)               │
└─────────────────────────────────────┘
```

All variable offsets are derived relative to the byte after `ListHeader`:

```text
orderBytes   = orderTrackIdCount * sizeof(TrackId)
nameOffset   = orderBytes
descOffset   = nameOffset + nameLength
filterOffset = descOffset + descLength
logicalSize  = sizeof(ListHeader) + filterOffset + filterLength
recordSize   = alignUp4(logicalSize)
```

The serializer emits exactly that size and fills the final zero through three padding bytes with zero.
The reader rejects multiplication/addition overflow, truncated fields, a non-canonical total size, nonzero padding, and hidden trailing bytes.
Text fields retain an independent 65,535-byte product limit; the 32-bit physical lengths do not authorize multi-gigabyte user text.

## Resource and dictionary records

Resource values are raw blob bytes with no header.
Dictionary values are raw UTF-8 bytes with no header or terminator.
Dictionary rows created for a referencing record are written in the same LMDB transaction.

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
`ListStore::Writer::create()` and `update()` repeat the complete structural gate before issuing an LMDB mutation.
An invalid payload returns `CorruptData`, and update leaves the prior value unchanged.

Read views perform one constant-time structural gate that proves the fixed header and all derived slices remain inside the record.
`TrackView` gates its hot and cold sides independently because callers may deliberately load only one side.
`isHotValid()` and `isColdValid()` are always legal and report whether the corresponding loaded side passed its gate.
A decoded hot or cold accessor requires that side to be valid; calling it for an absent or structurally invalid side is a programmer error that fails fast through `gsl_Expects`.
Raw diagnostic access remains available for the exact bytes supplied to the view.
An absent optional block inside a valid cold side is not an invalid tier: classical, cover-art, and custom-metadata proxies remain legal and empty, with their documented optional-block defaults.
Semantic in-bounds corruption is reserved for diagnostic deep verification and does not add a per-row scan.

A directly constructed invalid `ListView` retains its raw-view safety behavior: `isValid()` is false and decoded fields are empty or invalid.
That `ListView` behavior is not the `ListStore` absence contract.
For `ListStore::Reader::get()` and `ListStore::Writer::get()`, `nullopt` means only that the key is absent.
A structurally invalid stored value throws `library::detail::LibraryException` carrying `CorruptData`; dereferencing a List iterator does the same.
Callers therefore cannot mistake storage corruption for a missing List or silently omit a corrupt row from a full scan.
An exception during a write transaction unwinds through the transaction owner and aborts every uncommitted mutation.

Every store view borrows its bytes from the active LMDB transaction.
It must not outlive that transaction.
Within a write transaction, a subsequent database mutation may invalidate an earlier borrowed value even when the C++ `ListView` object is still in scope.
Code that writes after reading a List must first copy every needed scalar, string, rank vector, or serialized payload, or reacquire the List after the intervening write; it must never dereference the old view afterward.

Record validation operates inside the mapped-storage fault-containment limit defined by the [LMDB operation specification](../../../spec/storage/lmdb-operation.md#failure-and-cancellation).
It cannot turn an underlying mapped-file fault into a recoverable record-validation result.

## Compatibility and versioning

Opening a database whose metadata magic or stored library version is invalid returns `CorruptData`.
There is no migration path today; the current reset-and-rescan recovery instruction loses database-only user-authored state and must be treated as an explicit destructive fallback rather than a reconstruction guarantee.

Version `5` also gates the interpretation of saved List `filter` text and `orderTrackIds` rank semantics.
The exact accepted surface belongs to the [predicate language reference](../../query/predicate-language.md), and membership meaning belongs to the [predicate evaluation specification](../../../spec/query/predicate-evaluation.md).

Any incompatible key, record, enum encoding, slot meaning, signature-algorithm, stored List predicate, or saved-order interpretation change must increment `kLibraryVersion`.
A predicate change is incompatible when it expands the storable surface beyond what an existing same-version reader accepts, or when it can alter whether existing filter text parses or compiles, what it binds to, or which tracks it matches, even if `ListHeader` and its stored bytes do not change.
An explicitly tested future migration may replace reset-and-rescan recovery for an old version only when it reads the old predicate contract, converts or validates every affected filter atomically, and updates the metadata version after the converted data is valid; the target still has an incremented `kLibraryVersion`, and no such migration exists today.
There is no version-4 reader or in-place migration.
Opening a version-4 environment fails the exact version gate; preserving its user-authored data requires an explicit portable export performed by a compatible build before opening the library with version 5.
Transaction-local dictionary publication does not change the row shape or library version; it assumes a freshly created host-local index and adds no legacy-layout migration or validation path.

## Implementation authority

- [`MetadataLayout.h`](../../../../include/ao/library/MetadataLayout.h) owns magic, version, and metadata sizes.
- [`TrackLayout.h`](../../../../include/ao/library/TrackLayout.h), [`ListLayout.h`](../../../../include/ao/library/ListLayout.h), and [`FileManifestLayout.h`](../../../../include/ao/library/FileManifestLayout.h) own binary structs and static size checks.
- [`MusicLibrary.cpp`](../../../../lib/library/MusicLibrary.cpp) owns environment and named-database creation.
- [`ReadTransaction.h`](../../../../include/ao/library/ReadTransaction.h) and [`WriteTransaction.h`](../../../../include/ao/library/WriteTransaction.h) own the public transaction capabilities.
- Store and builder implementations under [`lib/library/`](../../../../lib/library/) own key allocation and write validation.

## Test authority

- [`MusicLibraryTest.cpp`](../../../../test/unit/library/MusicLibraryTest.cpp) covers environment, metadata, revision, and version behavior.
- [`ListLayoutTest.cpp`](../../../../test/unit/library/ListLayoutTest.cpp), [`ListBuilderTest.cpp`](../../../../test/unit/library/ListBuilderTest.cpp), and [`ListViewTest.cpp`](../../../../test/unit/library/ListViewTest.cpp) lock the 20-byte header, field offsets, canonical packing, checked sizing, and padding gate.
- [`ListStoreTest.cpp`](../../../../test/unit/library/ListStoreTest.cpp) locks pre-mutation validation and fail-closed point-read, writer-read, and iteration behavior.
- Other layout and serialization tests under [`test/unit/library/`](../../../../test/unit/library/) lock the remaining record sizes, alignment, validation, and store behavior.

## Related documents

- [Resource blob](../../resource/blob.md)

- [Library architecture](../../../architecture/library.md)
- [LMDB operation specification](../../../spec/storage/lmdb-operation.md)
- [Library access and mutation](../../../spec/library/runtime/mutation.md)
- [Library scan and audio identity](../../../spec/library/runtime/scan-and-identity.md)
- [List model](../model/list.md)
- [Predicate language](../../query/predicate-language.md)
- [Persistence and managed-state architecture](../../../architecture/persistence-and-managed-state.md)
