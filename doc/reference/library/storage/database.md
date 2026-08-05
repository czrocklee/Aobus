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
It is the sole public recoverable construction boundary for `MusicLibrary` and returns `Result<MusicLibrary>`; there is no throwing public constructor or exception compatibility path.
It validates the metadata/version gate and the dictionary, Track, List, and manifest invariants described below before exposing any store.
An absent metadata header initializes a library only when every named database is empty; data without that header is `CorruptData`, not a partially initialized new library.

The database is host-local rather than an interchange format.
It combines regenerable scan facts with user-authored lists, membership, curated metadata, tags, covers, custom metadata, and stable library/track identities; the complete environment is therefore not rebuildable from media files.
Its path must reside on a filesystem local to the host. Network filesystems and
shares are unsupported because neither the writer lease nor LMDB's mapped-file
locking provides cross-host safety for this database.
Integer keys use LMDB native word order and record structs are host-endian; [library YAML](../format/yaml.md) is the portable interchange surface.

## Transaction access

`MusicLibrary::readTransaction()` returns a move-only `ReadTransaction` that directly owns one native LMDB read snapshot.
Failure to begin the native transaction raises the library's general storage exception; this API has no recoverable typed-error channel.
`WritableMusicLibrary::acquire(MusicLibrary&)` returns the explicit move-only capability whose `writeTransaction()` factory returns a `WriteTransaction`; `MusicLibrary` exposes no public write-transaction factory.
The factory performs native begin and the transaction's one staged revision bump before exposing the wrapper.
Either failure releases the process writer gate and discards the transaction's lease-anchor reference, then raises the library's general storage exception instead of creating a recoverable authoring result.
The originating writable capability continues to hold its process session lease.
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
- Persisted dictionary, Track, and List integer keys are exactly four native bytes before conversion; dictionary keys are the dense range `1..N`, Track keys are nonzero matching hot/cold pairs, and List keys are nonzero.
- An aborted transaction-local dictionary tail has no durable identity and its ids may be reused by a later transaction.
- The dictionary persists id-to-string rows and rebuilds its string-to-id index in memory when opened; that same traversal validates key width, dense order, and text uniqueness before the store is exposed.
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

Every Track write uses immutable `TrackBuilder::PreparedHot` and `PreparedCold` values through the helpers in `TrackWrite.h`.
`TrackStore::Writer` accepts no caller-supplied record bytes and exposes no callback-filled or mutable LMDB span, so there is one record-writing path rather than a prepared path beside a raw one.
That also removes the defensive copy a caller span would need, because a reservation is already four-byte-aligned storage the canonical validators can view as typed records.
Writes reject the reserved zero `TrackId` as a `CorruptData` fault.
Complete `prepare()` and `serialize()` first run both pure hot and cold validators; single-side entry points run the validator for that side.
Those gates check every representable size and the canonical Track URI before dictionary interning, resource creation, or Track mutation begins, so a recoverable rejection adds no item-relative dictionary, resource, or Track delta.
That private zero-copy path reserves the hot value, fills every byte, validates it, and lets the reservation leave scope before reserving the cold value.
Updates follow the same per-side sequence.
Prepared writes fill a transaction-owned reservation and validate the complete bytes before the reservation leaves scope.
A canonical post-fill validation failure is an internal `InvalidState` fault; the root operation owner explicitly aborts the transaction before rethrowing it.

Create requires both canonical payloads and allocates the identity by appending to `tracks_hot`, then creates the same key in `tracks_cold`.
A cold-key conflict after hot allocation is `CorruptData` and rolls back the hot row.
After open establishes the exact-pair invariant, update helpers do not rescan the opposite database as a recovery check.
A single-side update checks its target row and returns `NotFound` without terminating the transaction when that row is absent.
`updatePreparedTrackRecord()` checks the hot target once before its first mutation, then relies on the established pair invariant for the cold replacement.
It changes both sides as one logical operation.
If any multi-side mutation reaches its first successful storage update and a later storage step fails, the mutation raises `lmdb::detail::TransactionFailure`; the root operation owner explicitly aborts before translating the failure to its enclosing `Result` boundary.

Expected no-mutation outcomes (`NotFound`, `Conflict` from an exclusive create, and integer-key exhaustion) are returned as `Result` values and leave the lower store transaction mechanically usable.
When such an error is the result of a root `WriteTransaction::apply()` body, root policy still aborts the complete transaction before returning it.
An LMDB fault from a mutation path is carried by `lmdb::detail::TransactionFailure` because continuing or committing could expose only a successful prefix of the logical operation.
`WriteTransaction::apply()` catches that marker, explicitly aborts every uncommitted row, terminalizes the wrapper, and returns the carried error.
`WriteTransaction::commit()` provides the same containment for a mutation fault during dictionary preparation.
No public runtime writer exposes a transaction that may be continued after either failure.

`TrackStore::Writer::remove()` deletes both physical keys without decoding their values and returns whether the Track pair existed; both absent returns false.
The write surface is not a damaged-database repair path because a one-sided physical Track cannot pass open.

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

Manifest point reads, iteration, and writes share one exact record validator.
Keys must be nonempty canonical `LibraryUri` bytes with the minimal zero padding needed to reach a four-byte multiple.
Values must be exactly 48 bytes, carry a nonzero Track id, a declared status, three zero reserved bytes, and either both parts of an audio identity or neither.
`FileManifestStore::Writer::put()` validates the key and value before its first LMDB mutation.
Only a point-read `NotFound` may be interpreted as absence, and a malformed point-read value is `CorruptData`.
Iterator dereference after a successful open assumes the validated store invariant; a malformed row raises the general `ao::Exception` infrastructure channel rather than skipping the row, returning partial output, or exposing a private library carrier.

## Validation rules

Builders are the only record producers and own overflow and structural validation before bytes reach a store.
`ListStore::Writer::create()` and `update()` repeat the complete structural gate before issuing an LMDB mutation; creation returns only the durable-candidate `ListId`, never a transaction-bound `ListView`.
An invalid payload returns `CorruptData`, and update leaves the prior value unchanged.

Open completes the metadata/version gate, validates dictionary key width, dense ids, and unique text while building its in-memory index, merge-checks the hot/cold Track key sets and validates canonical records plus every dictionary reference, validates every List key and record, and validates every manifest key and value.
These gates all complete before exposure; their internal evaluation order is not an error-precedence contract.
The first observed failure returns `CorruptData` for the complete open.
No `MusicLibrary`, runtime source, partial All Tracks membership, or salvage-row view is exposed.

Directly constructed read views perform one constant-time structural gate that proves the fixed header and all derived slices remain inside the record.
`TrackView` gates its hot and cold sides independently because callers may deliberately load only one side.
`isHotValid()` and `isColdValid()` are always legal and report whether the corresponding loaded side passed its gate.
A decoded hot or cold accessor requires that side to be valid; calling it for an absent or structurally invalid side is a programmer error that fails fast through `gsl_Expects`.
Raw diagnostic access remains available for the exact bytes supplied to the view.
An absent optional block inside a valid cold side is not an invalid tier: classical, cover-art, and custom-metadata proxies remain legal and empty, with their documented optional-block defaults.
The canonical write validator additionally checks exact size and zero padding, tag ids and bloom agreement, and cold block ordering.
Those linear checks do not add a per-row scan to normal decoded access.
`TrackBuilder::fromCompleteView()` requires both valid sides.
`fromHotView()` accepts a valid hot-only view but the resulting builder may serialize only hot data, preventing absent cold fields from being written back as defaults.

A directly constructed invalid `ListView` retains its raw-view safety behavior: `isValid()` is false and decoded fields are empty or invalid.
That `ListView` behavior is not the `ListStore` absence contract.
For `ListStore::Reader::get()` and `ListStore::Writer::get()`, `nullopt` means only that the key is absent.
A structurally invalid stored value after successful open throws the general `ao::Exception`; dereferencing a List iterator does the same.
Callers therefore cannot mistake storage corruption for a missing List or silently omit a corrupt row from a full scan.
An exception escaping a root write body is rethrown only after the transaction owner has explicitly aborted every uncommitted mutation.

`TrackStore::Reader::get()` and `visitTracks()` use a different partial-view contract: an absent row is skipped/returned as `nullopt`, while a loaded but structurally invalid hot or cold side is returned in a `TrackView` with its validity query false.
Visitors and other consumers must check the required side before decoded access; a contract failure after earlier visitor calls is not rolled back or reclassified as absence.

Every store view borrows its bytes from the active LMDB transaction.
It must not outlive that transaction.
Within a write transaction, a subsequent database mutation may invalidate an earlier borrowed value even when the C++ `ListView` object is still in scope.
Code that writes after reading a List must first copy every needed scalar, string, rank vector, or serialized payload, or reacquire the List after the intervening write; it must never dereference the old view afterward.

Record validation operates inside the mapped-storage fault-containment limit defined by the [LMDB operation specification](../../../spec/storage/lmdb-operation.md#failure-and-cancellation).
It cannot turn an underlying mapped-file fault into a recoverable record-validation result.

## Compatibility and versioning

Opening a database whose metadata magic or stored library version is invalid returns `CorruptData`.
There is no migration path today; the current reset-and-rescan recovery instruction loses database-only user-authored state and must be treated as an explicit destructive fallback rather than a reconstruction guarantee.
Safely detected malformed dictionary, Track, List, or manifest state likewise rejects open as a unit.
Preserving curation requires a usable YAML export or another backup made before damage; Aobus does not assume a damaged database can still be exported.
The Track write sequencing, validation, and return-value contracts do not change stored bytes, so they require neither a format-version increment nor a migration.

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
- [`TrackRecordValidation.cpp`](../../../../lib/library/TrackRecordValidation.cpp), [`FileManifestValidation.cpp`](../../../../lib/library/FileManifestValidation.cpp), and [`LibraryUriValidation.h`](../../../../lib/library/LibraryUriValidation.h) own canonical persisted-record validation.
- [`MusicLibrary.cpp`](../../../../lib/library/MusicLibrary.cpp) owns environment, named-database creation, and the complete open gate; [`DictionaryStore.cpp`](../../../../lib/library/DictionaryStore.cpp) establishes the dictionary representation while loading it.
- [`ReadTransaction.h`](../../../../include/ao/library/ReadTransaction.h) and [`WriteTransaction.h`](../../../../include/ao/library/WriteTransaction.h) own the public transaction capabilities.
- Store and builder implementations under [`lib/library/`](../../../../lib/library/) own key allocation and write validation.

## Test authority

- [`MusicLibraryTest.cpp`](../../../../test/unit/library/MusicLibraryTest.cpp) covers environment, metadata, revision, version, and dictionary/Track/List/manifest open integrity behavior.
- [`LibraryUriTest.cpp`](../../../../test/unit/library/LibraryUriTest.cpp) locks parsing and the allocation-free persisted canonical predicate to the same canonical spelling.
- [`ListLayoutTest.cpp`](../../../../test/unit/library/ListLayoutTest.cpp), [`ListBuilderTest.cpp`](../../../../test/unit/library/ListBuilderTest.cpp), and [`ListViewTest.cpp`](../../../../test/unit/library/ListViewTest.cpp) lock the 20-byte header, field offsets, canonical packing, checked sizing, and padding gate.
- [`ListStoreTest.cpp`](../../../../test/unit/library/ListStoreTest.cpp) locks pre-mutation validation and post-open fail-fast point-read, writer-read, and iteration behavior.
- [`FileManifestStoreTest.cpp`](../../../../test/unit/library/FileManifestStoreTest.cpp) locks manifest validation, point-read outcomes, and post-open iterator fail-fast behavior.
- [`TrackStoreRawLayoutTest.cpp`](../../../../test/unit/library/TrackStoreRawLayoutTest.cpp) locks record layout, load modes, and ordinary store behavior.
- [`TrackStoreIntegrityTest.cpp`](../../../../test/unit/library/TrackStoreIntegrityTest.cpp) locks reserved-id rejection and the canonical sweep over persisted records.
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
