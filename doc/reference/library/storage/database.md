---
id: library.database
---
# Library database

## Scope and version

This reference defines physical library format version `10`, gated by `ao::library::kLibraryVersion`.
It owns the LMDB environment, named databases, key encodings, record composition, size and alignment requirements, and version policy.
The environment uses the LMDB 1.0 file format; LMDB 1.0 and 0.9 cannot open each other's environments.
Opening an environment written by LMDB 0.9 fails with LMDB's version-mismatch diagnostic before the library version gate runs, and Aobus provides no migration.

Entity meaning belongs to the [track](../model/track.md) and [list](../model/list.md) references.
Scan and identity behavior belongs to the [scan and audio identity specification](../../../system/library/scan-and-identity.md).

## Code boundary

This surface belongs to the **core libraries** layer in the [system architecture](../../../system/overview.md).
`ao::library::MusicLibrary`, its stores, builders, views, and LMDB adapter dependencies live under `include/ao/library/` and `lib/library/`; application-runtime commands may coordinate them but do not own or redefine this format.

## Environment

The library is one LMDB environment at the database path passed to `MusicLibrary::open`; normal application composition supplies `<music-root>/.aobus/library`.
Application composition keeps only one live environment for a database path in each process, as required by LMDB; `MusicLibrary` has no canonical-path registry or duplicate-open admission mechanism.
Independent processes may open the same environment under LMDB's normal locking rules.
`MusicLibrary::open` uses `MDB_NOTLS`, allows eight named databases, and configures the LMDB reader table through `MusicLibrary::Options::maxReaders`.
That option is a requested table size, defaults to `512`, and skips `mdb_env_set_maxreaders` when zero.
Every commit is flushed to stable storage unless `MusicLibrary::Options::flushCommits` is cleared.
Application composition keeps the default through `rt::LibraryDurability::Durable`; test fixtures select `Unflushed`.
An unflushed commit survives a process crash, but an operating-system crash or power loss can lose the most recent commits or corrupt the library, so only disposable libraries use it.
The effective capacity comes from the persistent LMDB lock file: an opener that acquires the exclusive environment lock may grow a smaller table to the requested size, a concurrent opener adopts the existing size, and no opener shrinks a larger table.
Each live `MusicLibrary::readTransaction()` snapshot occupies one effective slot until destruction under `MDB_NOTLS`.
Exhausting that table follows the existing fatal transaction-begin contract owned by the [library architecture](../../../system/library/structure.md#failure-cancellation-and-lifetime-boundaries); `maxReaders` raises capacity but is not a concurrency throttle.
That map is the capacity this environment may grow into, not disk it occupies: the [LMDB adapter](../../../system/persistence/lmdb-operation.md) prepares the data file so allocation follows committed use, and a full map remains the point at which mutation fails with `StorageFull`.
`MusicLibrary::storageCapacity()` reports that capacity as `mapBytes` together with the `highWaterBytes` the database has needed; the high water is the peak page extent rather than live data, so deleting rows does not lower it.
Its `diskBytes` reports the allocated bytes of `data.mdb`, `lock.mdb`, and `.aobus-writer.lock`, excluding frontend state and unrelated descendants.

Capacity is either managed or pinned, and `MusicLibrary::Options` chooses which.

Managed capacity is the default and is what the application uses.
A fresh database opens at a floor, an existing one keeps the capacity it recorded, and each open raises the map when the recorded peak has come within a doubling of filling it, up to a 64 GiB ceiling.
The floor depends on what the map costs on the volume holding it: 2 GiB where the data file can hold a hole, so the capacity is reserved without occupying disk, and 1 GiB where it cannot, because there an empty library would occupy the whole floor immediately.
The dense figure is what the fixed map before managed capacity already claimed on such a volume, so growth is added on top rather than the resting footprint being raised.
Where the data file cannot hold a hole the map then grows by 256 MiB steps instead of doubling, because there a larger map is larger disk usage.
Capacity only ever grows: no open lowers a map a database already recorded, and no live environment is resized.
A mutation that exhausts the map rolls back and leaves the recorded peak untouched, so nothing asks the next open for more room; repeating that work needs the database to have grown for another reason.

`Options::pinnedMapBytes` instead pins the capacity at exactly that many bytes and disables growth, overriding both the floor and whatever the database recorded.
It is for callers that need one known capacity, including tests that mean to reach the end of a map.
It is the sole public recoverable construction boundary for `MusicLibrary` and returns `Result<MusicLibrary>`; there is no throwing public constructor or exception compatibility path.
It first requires byte-key flags for the main LMDB database, then enumerates that catalog before any named database is created and initializes the exact version-10 schema only when that catalog is empty.
LMDB 1.0 keys each named database's catalog entry by its name followed by a terminating NUL byte, so admission compares those exact keys and treats a row keyed by the bare name as an ordinary main-database record.
Fresh and existing admission open all seven named DBIs sequentially and exactly once in that initialization write transaction.
Existing admission opens `meta` into one source-private, read-only unvalidated token, reads the stable version prefix, and—only for the current version—consumes that same DBI into an `IntegerKeyDatabase` after exact flag validation; it never reopens `meta`.
The resulting integer-key and byte-key tokens remain internal to the library and are reused by later read and write transactions.
A nonempty environment must contain the existing `meta` database and metadata header; a partial schema or ordinary main-database record is `CorruptData`, not a partially initialized new library.
After the version gate accepts version 10, open requires exactly the seven named databases below, their exact key flags, the required header and id space records with at most the optional revision record beside them, and every local and cross-Store invariant described below before exposing any store.

The database is a native-format store rather than an interchange format.
It combines regenerable scan facts with user-authored lists, membership, curated metadata, tags, covers, custom metadata, and stable library/track identities; the complete environment is therefore not rebuildable from media files.
Its path must reside on a locally attached filesystem. Network filesystems and
shares are unsupported because neither the writer lease nor LMDB's mapped-file
locking provides cross-host safety for this database.
Integer keys and record fields are little-endian in LMDB's native word order, and the format is defined only for 64-bit hosts: [`MetadataLayout.h`](../../../../include/ao/library/MetadataLayout.h) refuses to compile for another byte order or word size, and every persisted struct asserts its size, field offsets, and absence of padding.
[Library YAML](../format/yaml.md) is the interchange surface across format versions.

## Library transaction consequences

The [LMDB operation specification](../../../system/persistence/lmdb-operation.md) owns generic transaction begin, commit, abort, cursor lifetime, mutation-failure channels, and copied-data operations.
The [library architecture](../../../system/library/structure.md#physical-storage-and-capabilities) owns the root `WriteTransaction::apply()` procedure, writer capability, process lease, candidate revision, and publication boundary.
This reference records the format-specific consequences.

`MusicLibrary::readTransaction()` returns a move-only coherent LMDB snapshot carrying the metadata header and committed revision read from that snapshot.
The specialized stores are const read services: their readers use a read transaction, a write transaction for pre-operation inspection, or the active `LibraryWrite` context for one coherent in-operation view.
`MusicLibrary` exposes logical metadata-header values rather than a physical Metadata Store handle.
A store accepts only a transaction minted by the same stable `MusicLibrary`; cross-library metadata, revision, or record access fails before exposing a fact.
Mapped record views borrow bytes from that transaction and follow the [view safety rules](#view-safety). Committed dictionary text instead has the store-owned lifetime described by the [library architecture](../../../system/library/structure.md#physical-storage-and-capabilities).
These semantic wrappers add no second storage transaction or read-path heap allocation.
A failure to begin a snapshot is fatal for an already admitted live library because no recoverable alternate snapshot exists.

`MusicLibrary` exposes no public write-transaction factory.
A separately acquired `WritableMusicLibrary` is the only authority that can create a write transaction, and production mutation receives only callback-scoped logical Track and List ports through `LibraryWrite`.
Physical Store writers, native transaction handles, and mutable LMDB reservations remain inaccessible to production callers except for the source-private Track encoder described below.
An active transaction keeps the non-blocking `<database-path>/.aobus-writer.lock` lease alive even if the originating writable capability is destroyed.
The lock file has no governed payload and is not part of format version `10`; it must not be removed while a writable process is active.

The logical Track writer preserves the format's hot/cold, manifest, dictionary, and Resource relationships while it:

- validates a `TrackBuilder` for representability and existing Resource references without writing dictionary or Resource rows;
- creates one hot/cold Track pair together with its manifest row;
- updates complete, hot-only, or cold-only Track data while retaining its existing URI;
- replaces Track data and its existing manifest facts together;
- updates only file status or audio identity while retaining the existing URI-to-Track binding;
- relinks by changing the cold URI, removing the old manifest key, and creating the new binding as one operation;
- deletes one Track together with its manifest row; or
- clears Tracks and the manifest together.

The logical List writer accepts `ListBuilder`, prepares its physical row internally, and creates or updates it only after validating the live parent chain.
Update first proves that its target exists, so the physical LMDB upsert primitive cannot create a caller-selected List id.
An ordinary delete returns `NotFound` for an absent id and `Conflict` while a child exists.
Explicit subtree deletion discovers the complete live subtree, deletes children before parents, and returns the deleted ids in root-first discovery order.
Clear removes the complete List graph coherently and keeps its ids allocated.
Saved-order Track ids must not exceed the highest Track id ever allocated, including ids the same transaction allocated; a larger id returns `InvalidInput`, because it would bind a rank to whichever Track later receives that id.
Filter bytes remain opaque throughout these storage operations.

## Named databases

| Database | Key | Value |
|---|---|---|
| `meta` | Fixed integer record id | Record `1`: `MetadataHeader`; record `2`: one `std::uint64_t` library revision; record `3`: `IdSpace`. |
| `tracks_hot` | `TrackId` integer | `TrackHotHeader`, tag-id array, title bytes. |
| `tracks_cold` | `TrackId` integer | `TrackColdHeader`, optional block payloads, URI bytes. |
| `lists` | `ListId` integer | `ListHeader`, order-track-id array, name/description/filter bytes, zero padding. |
| `resources` | Digest-derived `ResourceId` integer | 36-byte `ResourceDescriptor`. |
| `dictionary` | `DictionaryId` integer | Scalar-valid UTF-8 NFC bytes without a terminator. |
| `file_manifest` | Root-relative URI padded to a four-byte multiple | `FileManifestHeader`. |

One database slot remains spare.

## Keys and identifiers

All integer identifiers are 32-bit values and reserve `0` as invalid.

- Track and List ids are never reused. A writer allocates one above the higher of the table's last stored key and the id space's last committed id, so the first id is `1`, and deleting the highest row or clearing the table leaves that id allocated. Exhaustion returns `ResourceExhausted` and is permanent for that database, even after clearing; a YAML export restored into a new database starts a fresh id space; List ids stop at `UINT32_MAX - 1`, because `UINT32_MAX` is never a stored List id, which leaves it free for frontends to name a virtual List.
- A track is appended to `tracks_hot` first and written to `tracks_cold` under the same id.
- A resource key starts from the first four bytes of the content's SHA-256 digest read big-endian, remaps zero to one, and linearly probes with digest comparison, stopping at the first empty slot; identical content reuses the existing id.
- Dictionary ids produced by current writers are the dense committed range `1..entryCount`; input text is normalized to NFC before identity lookup, new canonical text receives the next id, canonically equivalent text reuses its existing id, and committed ids are never deleted or rebound.
- Persisted dictionary, Track, and List integer keys are exactly four native bytes before conversion; dictionary keys are the dense range `1..N`, Track keys are nonzero matching hot/cold pairs, and List keys are nonzero and below `UINT32_MAX`.
- An aborted transaction-local dictionary tail has no durable identity and its ids may be reused by a later transaction.
- The dictionary persists id-to-string rows and rebuilds its string-to-id index in memory when opened; that same traversal validates key width, dense order, scalar-valid UTF-8 NFC text, and canonical text uniqueness before the store is exposed.
- Manifest keys are normalized root-relative URI bytes, at most 1,976 bytes (`LibraryUri::kMaxLength`), then zero-padded to a four-byte multiple; a longer URI returns `ValueTooLarge`. LMDB 1.0 derives its maximum key size from the page size of the host that created the database: 1,978 bytes for 4 KiB pages, the smallest page size of a supported host. The URI limit is the largest four-byte multiple within it, and open returns `NotSupported` for a database whose LMDB key limit is smaller, such as one created with smaller pages.

## Metadata records

`MetadataHeader` is 40 bytes and contains:

| Field | Representation |
|---|---|
| `magic` | `0x42534C52` |
| `libraryVersion` | Unsigned 32-bit format version. |
| `flags` | Unsigned 32-bit flags; must be zero. |
| `reserved` | Unsigned 32-bit word at offset 12; must be zero. |
| `createdTime` | Millisecond system timestamp at offset 16. |
| `libraryId` | 16 UUID bytes at offset 24; must not be all zero (the nil id). |

Every header byte is a named field, so no indeterminate padding reaches storage.
Creating the environment draws a random non-nil `libraryId`, and nothing rewrites the header afterwards.
The id names this database and its id history, so YAML restore never transfers it: a database rebuilt from a backup keeps its own id, distinct from the original's.

The revision record is an unsigned 64-bit integer.
Absence means the initial committed revision `0`; a present record must be exactly eight bytes and contain neither `0` nor `UINT64_MAX`.
The maximum valid committed value is `UINT64_MAX - 1`, a physically unreachable exhaustion sentinel for this desktop application's mutation rate.
A write transaction computes the next value in memory from the durable revision visible to its already-acquired LMDB writer snapshot, exposes it as its candidate revision, and persists it in that same native transaction immediately before commit.
Abort, pre-commit rejection, and failed commit leave the previous durable revision unchanged.

The id space record is required and is exactly 8 bytes:

| Field | Representation |
|---|---|
| `lastTrackId` | Unsigned 32-bit highest Track id ever committed, at offset 0. |
| `lastListId` | Unsigned 32-bit highest List id ever committed, at offset 4; at most `UINT32_MAX - 1`, the highest allocatable List id. |

Creating the environment writes the record with both marks at zero.
A write transaction reads it beside the revision, raises each Track and List writer's allocation floor to its mark, and, when either mark moved, writes the record before the revision.
A committed mark covers every id the transaction allocated, including ids it deleted again, and every key it stored, so a supported writer cannot commit a row above its mark.
Clearing the Track or List table keeps the marks, so a cleared id is never allocated again.
Abort and failed commit leave the record unchanged, so ids allocated by a transaction that never published may be allocated again.
Within one database, an id therefore names at most one object over its lifetime, and `libraryId` tells one database from another.
The guarantee does not survive copying the database file: an older copy put back in place rolls the marks back, and two copies used side by side share a `libraryId` while their id histories diverge, so either can later allocate an id the other already used.

## Track records

`TrackHotHeader` is 36 bytes, four-byte aligned, and contains the filter/sort working set: tag bloom, artist/album/genre/album-artist/composer dictionary ids, sample rate, year, title length, tag-blob length, bit depth, and codec.
The tag-id array follows the header, followed by title bytes.

`TrackColdHeader` is 32 bytes, four-byte aligned, and contains duration, bitrate, track/disc numbers and totals, five block offsets, URI offset/length, channels, and one reserved byte.
Defined block slots are written in slot order:

| Slot | Payload |
|---|---|
| `0` cover art | Zero or more eight-byte `CoverArtEntry` values: `ResourceId`, picture type, three reserved bytes. |
| `1` work | One 12-byte `TrackWorkBlock`: work and movement dictionary ids, then two unsigned 16-bit movement number/total values. |
| `2` custom metadata | Eight-byte header, eight-byte entries, then value bytes. |
| `3` performance | One 12-byte `TrackPerformanceBlock` prefix, followed by eight-byte `TrackCreditEntry` values. |
| `4` | Last unused cold slot; zero. |

An absent payload has offset zero.
Every present payload starts on a four-byte boundary, and URI bytes follow the block area.
The whole cold record, including the header, every present block, the URI, and padding, is at most 65,535 bytes (`kTrackColdRecordSizeLimit`); because the record is padded to a four-byte multiple, the largest stored record is 65,532 bytes.
Cover-art references, the work block, custom metadata, the recording date and credit references, and the URI share that one budget; no block or value has a separate allowance.
Only inline bytes count: cover-art image bytes are Resources, and work, credit, and custom-metadata key text is stored in the dictionary, so each cover-art reference, credit, and custom-metadata entry costs eight bytes.
In practice, custom-metadata values dominate the record; the URI is separately capped at 1,976 bytes (`LibraryUri::kMaxLength`) but still consumes the shared budget.
Track preflight sizes these components in physical order and rejects a record that would exceed the limit with `ValueTooLarge`, naming the first component that crosses it, before any mutation; open applies the same limit and rejects a larger record as `CorruptData`.
The fixed per-track value cost is 68 bytes before arrays, blocks, title, and URI bytes.
Inline title and custom-metadata value bytes are scalar-valid UTF-8 in NFC.
Every dictionary-backed metadata field, tag, and custom-metadata key inherits the same text invariant from its dictionary row.
The URI is filesystem identity and is explicitly outside this text-normalization contract.

Work structure and performance are independently optional.
The work block is omitted when both ids and both movement numbers are zero.
Performance is omitted iff RecordingDate is absent and all four credit segments are empty; a date-only or credits-only performance is valid without a work block.
Composer, artist, album artist, and the existing Year retain their hot access paths rather than moving to a logically related cold block.
These blocks are storage domains, not Work or Performance entities or recording identities.

The performance prefix has this exact layout; integers use the format's little-endian byte order:

| Byte offset | Value | Bytes |
|---:|---|---:|
| `0` | Recording year, unsigned 16-bit | `2` |
| `2` | Recording month, unsigned 8-bit | `1` |
| `3` | Recording day, unsigned 8-bit | `1` |
| `4` | Conductor entry count, unsigned 16-bit | `2` |
| `6` | Ensemble entry count, unsigned 16-bit | `2` |
| `8` | Soloist entry count, unsigned 16-bit | `2` |
| `10` | Reserved unsigned 16-bit word; zero | `2` |
| `12` onward | Conductor, Ensemble, Soloist, Performer segments: name id, optional role id | `8` per entry |

An absent recording date is exactly four zero bytes.
Year zero requires month and day zero; a present year is `1..9999`, month/day zero retain partial precision, and supplied components must form a valid proleptic Gregorian date.
There is no date flag or inferred value from the hot Year.

A credit name id must be nonzero and resolve to nonempty, ASCII-trimmed, scalar-valid UTF-8 NFC text.
Role id zero is absence; every nonzero role id must resolve to the same admitted nonempty text form.
All other nonzero dictionary ids must also resolve.
Ids identify supplied text, not entities; duplicates and within-kind order are retained.
Kind comes from the physical segment, not an entry member or dictionary label. Credits are the sole participant authority; category fields are projections.

The next present slot offset, or `uriOffset` for the last block, supplies the performance boundary.
Its length must be exactly `12 + 8 * N`: at least 12 bytes, with the remaining length divisible by eight and no tail padding.
The first three `sectionCounts` sum, widened before addition, must not exceed `N`; Performer count is `N` minus that sum.
Unsigned 16-bit counts leave the whole cold-record byte limit as the effective cardinality bound, not a 255-entry category cap.
A present prefix without date or entries is noncanonical and rejected, as is a logically empty work block.
A structurally valid changed count can reclassify entries: this nonredundant encoding cannot detect every semantic corruption.
The five-offset directory and both fixed headers remain unchanged.
Compute multiplication, additions, and alignment in a wide checked type before narrowing, including URI and every retained block in the full merged record.
This layout is not a general compression or performance guarantee; first-entry access remains O(1), but timings, heap use, and RSS do not follow from encoded size.

Production callers pass a `TrackBuilder` to the logical writer returned by `LibraryWrite::tracks()`.
The writer performs preparation itself and keeps the resulting immutable `TrackBuilder::PreparedHot` and `PreparedCold` values inside `ao_library`.
This binds resolved dictionary ids and Resource descriptor creation, from hashed bytes or from a declared descriptor, to the same library and transaction that performs the physical write; caller-supplied existing Resource ids must already exist in that write snapshot.
The physical `TrackStore::Writer` accepts no caller-supplied record bytes and exposes no mutable LMDB span, and it is unavailable to production callers, so there is one record-writing path rather than a prepared path beside a raw one.
Public LMDB create, update, and append operations accept only copied input bytes.
The Track writer is the only production consumer that reaches the integer writer's private reservation primitives through source-private `lmdb::detail::ReservationWriterAccess`; its synchronous no-throw encoder receives the four-byte-aligned reservation only for that invocation, and the span is never returned to the Store.
Writes reject the reserved zero `TrackId` as a `CorruptData` fault.
Complete internal preparation first runs both pure hot and cold validators; single-side logical updates run the validator for that side.
Private serialization helpers used by representation tests follow the same preflight rules.
Those gates validate scalar UTF-8, canonical Track URI, representable inline post-NFC sizes, and complete encoded record bounds before dictionary interning, Resource creation, or Track mutation begins.
Already-NFC inline scalar input uses a quick check to avoid a temporary normalized allocation during size preflight.
Credit preflight independently checks valid kinds, trimmed nonempty names, name/role UTF-8, date validity, counters, and full cold-record bounds, without requiring upstream normalization.
Preparation stably groups the four fixed kinds, trims and interns names/nonempty roles, and obtains NFC from dictionary admission.
The owning-input setter still retains copied borrowed descriptors; all supplied text must survive through preparation.
Failures after interning starts abort the enclosing root write; there is no per-call rollback or permission to continue and commit that transaction, and private dictionary changes are not published early.
Prepared values own every byte their encoder emits, including performance entries and the exact date precision.
They validate builder input and representability rather than encoded bytes: a prepared Track side is a typed zero-copy snapshot and does not retain a second serialized record.
That private zero-copy path reserves the hot value, fills every byte, and validates it before its encoder callback returns, then repeats the sequence for the cold value.
Updates follow the same per-side sequence.
Prepared writes fill and validate transaction-owned storage entirely inside the synchronous encoder invocation.
A canonical post-fill validation failure violates the encoder's `AO_ENSURES` postcondition and aborts immediately.

Create requires both canonical payloads and allocates the identity by appending to `tracks_hot`, then creates the same key in `tracks_cold`.
After open proves matching key sets, a cold-key conflict after hot allocation is an invariant breach and aborts immediately; supported writers cannot create that state.
Update helpers rely on that admitted exact-pair invariant rather than rescanning the opposite database as recovery.
A `Both` point lookup still probes both sides so it distinguishes a normal miss, where neither row exists, from an invariant breach, where exactly one row exists.
A single-side update checks its target row and returns `NotFound` without mutating when that row is absent.
A complete update checks the hot target before its first mutation and changes both sides as one logical operation.

`NotFound`, exclusive-create `Conflict`, and integer-key exhaustion are no-mutation outcomes at the physical operation that reports them.
A later failure after any part of a logical multi-store mutation has been staged aborts the complete library root; no public runtime writer may continue or commit a failed root.
The [LMDB operation specification](../../../system/persistence/lmdb-operation.md#writes) owns the private mutation-fault carrier and adapter failure channels, while the [library architecture](../../../system/library/structure.md#physical-storage-and-capabilities) owns whole-root rollback.

The logical Track delete first proves the complete Track and matching manifest binding, then deletes the manifest and both physical Track keys.
An absent Track returns successful `false`; a present Track with missing or mismatched
cross-Store evidence violates the admission-and-writer invariant and aborts rather than
presenting the live database as recoverably corrupt.
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
Text fields retain an independent 65,535-byte post-NFC product limit; the 32-bit physical lengths do not authorize multi-gigabyte user text.

`ListBuilder` owns copies of its name, description, and filter inputs, so a logical mutation may stage the semantic value without retaining a caller or LMDB view.
`ListBuilder::prepare()` validates scalar UTF-8, normalizes name and description to NFC, preserves filter bytes, then snapshots one immutable prepared value after checking the product limits, exact canonical layout, nonzero unique saved-order ids, and every derived extent.
The filter field is syntactically opaque at this boundary: any scalar-valid UTF-8 text within the size limit is locally valid, and no query parse or compile occurs.
`ListStore::Writer` accepts only the prepared value and passes its owning encoded bytes through the ordinary copied-data database overload.
It does not serialize again or allocate and rebuild a second saved-order uniqueness set before copying those bytes.
Creation allocates the nonzero List key; update requires a nonzero key.
Parent existence and parent-cycle checks are cross-row logical-writer rules and are not part of this local prepared-record gate.

## Resource and dictionary records

A resource value is exactly 36 bytes: a 32-byte SHA-256 digest followed by a 32-bit content length in the machine's own byte order.
Like every other record here, the row is the struct's object representation written whole rather than a chosen encoding; a database file is bound to the architecture that created it either way.
The row describes content and holds none of it; the content lives in the media files the library indexes, and the [cover-art delivery specification](../../../system/resource/cover-art-delivery.md) owns how it is read and verified.
The `resources` database is append-only in practice: no production path deletes a row, because `create` stops probing at the first empty slot and a hole in a collision chain would let a later create mint a second row for one digest.
Rows a track no longer references are retained and remain valid.

Dictionary values are scalar-valid UTF-8 NFC bytes with no header or terminator.
Dictionary rows created for a referencing record are written in the same LMDB transaction.

## Manifest records

`FileManifestHeader` is 52 bytes and four-byte aligned:

| Offset | Field | Representation |
|---:|---|---|
| `0` | Track | `TrackId`. |
| `4` | File size | Low/high unsigned 32-bit halves of one unsigned 64-bit value. |
| `12` | Modification-time seconds | Low/high 32-bit halves of one signed 64-bit seconds value. |
| `20` | Modification-time nanoseconds | Unsigned 32-bit fraction of the same second, below `1000000000`. |
| `24` | Audio payload length | Low/high unsigned 32-bit halves of one unsigned 64-bit value. |
| `32` | Audio signature | 16-byte XXH128 canonical big-endian serialization. |
| `48` | Status | `Available = 0`, `Missing = 1`, or `Error = 2`. |
| `49` | `hasMtime` | `1` when a modification time is stored, `0` when it is absent. |
| `50` | Padding | Two zero bytes. |

The modification time is one 12-byte `FileTimestamp` instant: a signed 64-bit seconds count from the Unix/POSIX epoch `1970-01-01 00:00:00 UTC`, excluding leap seconds, plus a normalized nanoseconds fraction in `[0, 1000000000)`.
The seconds value is stored in low/high 32-bit halves like the other 64-bit manifest fields, but it is a signed count, so instants before 1970 are representable.
The stored value is a defined instant with no UTC-versus-local meaning; timezone is a display concern only.
Absence is a separate state, not a clock value: `hasMtime = 0` requires both seconds halves and the nanoseconds word to be zero, while `hasMtime = 1` requires nanoseconds below `1000000000`.
The epoch instant `seconds = 0, nanoseconds = 0` with `hasMtime = 1` is therefore a present value distinct from absence, and the C++ API represents the state as `std::optional<FileTimestamp>`.

Zero payload length together with an all-zero signature means pending audio identity.

Manifest point reads, lower-bound seeks, iteration, and writes share one exact record validator.
Keys must be nonempty canonical `LibraryUri` bytes with the minimal zero padding needed to reach a four-byte multiple.
Values must be exactly 52 bytes, carry a nonzero Track id, a declared status, two zero padding bytes, either both parts of an audio identity or neither, and a modification-time state consistent with its `hasMtime` flag: an absent flag requires zero seconds and nanoseconds words, and a present flag requires nanoseconds below `1000000000`.
Preparation is split because a creating write only allocates the owning Track id once its Track record exists.
`FileManifestBuilder::validate()` parses the URI, applies the canonical key validator and every record fact that does not depend on that binding, and snapshots a zero-id header without allocating a serialized payload.
`FileManifestBuilder::Unbound::bind()` supplies the nonzero owning Track id, cannot fail, and reapplies the complete payload validator as its postcondition, so every stored value still passes one exact validator.
It consumes its unbound value exactly once; rebinding a consumed value surrenders the validated key and is a caller call-order violation that fails through `AO_EXPECTS`.
`FileManifestStore::Writer::put()` accepts only the bound prepared value and passes its owning encoded bytes through the ordinary copied-data database overload.
`FileManifestStore::Reader::lowerBound(uri)` validates that canonical URI key and returns the first manifest whose URI is not less than it in LMDB byte order.
Only a point-read `NotFound` may be interpreted as absence.
Point reads and iterator dereference after a successful open assume the validated Store invariant; a malformed row fails through `AO_INVARIANT` rather than being skipped, returned as partial output, or exposed through a private library error carrier.

## Validation rules

Production callers cannot submit serialized Track, List, or manifest byte spans to their structured Store writers.
The logical Track port accepts `TrackBuilder` and `FileManifestBuilder`, the logical List port accepts `ListBuilder`, and both own their private preparation.
Manifest-only Track updates likewise accept the owning `TrackId` plus `FileManifestBuilder`; the port derives and preserves the live URI-to-Track binding itself.
Track preflight and List preparation reject malformed UTF-8 as `InvalidInput`, normalize admitted library text to NFC, and return recoverable validation errors before their first related mutation; manifest preparation preserves the separate native URI identity contract.
Passing an invalid prepared value is impossible through the public construction surface.
Once Track preparation starts interning dictionary text or creating Resource descriptors, any later failure reaches the root operation boundary and aborts the complete transaction.
Track encoders validate the bytes they fill with the same canonical local validators used by open and treat a mismatch as an `AO_ENSURES` failure.
Prepared List and manifest values already own their validated canonical bytes, so their writers use the copied-data overload without another serialization or allocating validation pass.

After main-database admission, existing-schema open uses the source-private unvalidated `meta` token to read only the stable eight-byte metadata prefix needed for magic and version.
A valid non-current version returns `NotSupported` before version-10 catalog closure, exact header size, named-database flags, or extra-database checks; no conversion is attempted, even when an existing named database's key flags differ from the current schema.
The main database's byte-key flags are admitted before safe catalog enumeration and therefore before the metadata version lookup.
For version 10, the header is exactly 40 bytes with zero flags, a zero reserved word, and a non-nil library id, and the catalog is exactly the seven named databases above.
Admission then requires exact `MDB_INTEGERKEY` flags before consuming the existing `meta` DBI as an `IntegerKeyDatabase`, opens the remaining integer-key databases as `IntegerKeyDatabase`, opens `file_manifest` as `ByteKeyDatabase` with no key flags, and permits no unvalidated token to escape initialization.
The typed `meta` token contains only header record `1`, optional revision record `2`, and the required 8-byte id space record `3`, whose `lastListId` is at most `UINT32_MAX - 1`.

The current-schema gate then validates Resource keys as nonzero four-byte ids and Resource values as exactly 36 parseable descriptor bytes, requires every stored digest to be distinct, and requires every row to be reachable from its digest's initial key along an unbroken run of occupied slots; orphan Resources and orphan dictionary rows are accepted.
A row whose key is not the first free slot at or above its digest's initial key is `CorruptData`, with the empty slot before it as the cause; a long collision cluster and a cluster spanning the wrap from the maximum key to `1` are both valid.
Reachability is checked over the occupied key runs the same traversal already collects, so it stays linear in the number of rows.
It validates dictionary key width, dense ids, scalar-valid UTF-8 NFC, and unique canonical text while building the in-memory index.
It merge-checks the hot/cold Track key sets, validates canonical records including scalar-valid UTF-8 NFC inline text plus every dictionary and Resource reference, and proves a strict Track-to-manifest bijection.
For performance records it rejects malformed prefix/tail bounds, a widened count sum greater than the physical entry count, a nonzero reserved word, noncanonical empty prefixes, invalid partial dates, zero or unresolved credit name ids, unresolved role ids, and empty or untrimmed referenced name/role text.
Dictionary admission has already proved scalar UTF-8 and NFC, so checking the extra credit text rules requires no per-entry normalization or temporary string.
The bijection first compares row counts, then performs one canonical manifest point read for each Track URI and requires the manifest's Track id to equal that Track id.
Equal counts, unique Track ids, and the exact point matches prove that no extra manifest row exists without allocating a Track- or manifest-sized set; duplicate Track URIs and duplicate manifest bindings cannot pass.
It validates every List key and local record, including scalar-valid UTF-8 NFC text, requires each non-root parent to exist, and rejects parent cycles using memory proportional to the List count.
Every Track key must be at most the id space's `lastTrackId`, and every List key at most its `lastListId` and below `UINT32_MAX`.
Saved-order Track ids must be nonzero, unique, and at most `lastTrackId`, but they may be stale or currently absent because saved rank is intentionally retained outside current membership.
Stored filter bytes remain opaque and are not parsed or compiled by database admission.
These gates all complete in one coherent initialization transaction before exposure; their internal evaluation order after the version gate is not an error-precedence contract.
The first observed failure returns `CorruptData` for the complete open.
No `MusicLibrary`, runtime source, partial All Tracks membership, or salvage-row view is exposed.
The admission algorithm is linear in the number and total byte size of persisted rows.
Track-to-manifest closure adds one manifest point lookup per Track and constant Track-sized auxiliary storage; List topology adds linear auxiliary storage in the number of Lists.
Tests lock the operation-count slope from `N` to `2N`, and a manual 100,000-Track evidence run records wall time and peak resident memory without imposing a machine-dependent CI time threshold.

### View safety

Directly constructed read views perform one constant-time structural gate that proves the fixed header and all derived slices remain inside the record.
`TrackView` gates its hot and cold sides independently because callers may deliberately load only one side.
`isHotValid()` and `isColdValid()` are always legal and report whether the corresponding loaded side passed its gate.
A decoded hot or cold accessor requires that side to be valid; calling it for an absent or structurally invalid side is a programmer error that fails fast through `AO_EXPECTS`.
Raw diagnostic access remains available for the exact bytes supplied to the view.
An absent optional block inside a valid cold side is not an invalid tier: work and performance views, and cover-art and custom-metadata proxies, remain legal and empty, with their documented optional-block defaults.
The performance read gate checks prefix bounds/alignment, whole-entry tail stride, the widened sum of the first three section counts against `N`, and RecordingDate validity in constant time, before creating subspans or subtracting the Performer count.
It never materializes the list or scans its members; invalid layouts expose no out-of-bounds segment.
Because date formatting and comparison require a valid date, a stored invalid date makes the cold side invalid instead of reaching them.
Reserved-word validity, name ids, and canonical omission of logically empty performance blocks are enforced by deep validation at write/open time, not repeated by the shallow read gate.
A directly constructed raw view may therefore expose a present all-zero prefix; structural validity does not prove semantic validity.
`PerformanceView::credits()` and `credits(kind)` borrow physical id-entry spans, sharing the cold record's transaction/buffer lifetime; consumers supply logical kind from segment context.
The canonical write validator additionally checks exact size and zero padding, tag ids and bloom agreement, cold block ordering, and every credit name id.
Those linear checks do not add a per-row scan to normal decoded access.
`TrackBuilder::fromCompleteView()` requires both valid sides.
It preserves every work/performance field, exact date precision, and canonical credit name/kind/role entry when rebuilding a complete record.
`fromHotView()` does not fetch cold fields or list entries and the resulting builder may serialize only hot data, preventing absent cold fields from being written back as defaults.

A directly constructed invalid `ListView` retains its raw-view safety behavior: `isValid()` is false and decoded fields are empty or invalid.
That `ListView` behavior is not the `ListStore` absence contract.
For `ListStore::Reader::get()` and `ListStore::Writer::get()`, `nullopt` means only that the key is absent.
A structurally invalid stored value after successful open fails through `AO_INVARIANT`; dereferencing a List iterator does the same.
Callers therefore cannot mistake storage corruption for a missing List or silently omit a corrupt row from a full scan.
The check aborts rather than unwinding into an application catch boundary because the open-time proof or a supported writer invariant has been violated.

`TrackStore::Reader` itself is the complete hot-and-cold input range: its public `begin()` and `end()` traverse paired Track rows, and it has no public modeful `begin()` or `end()` overload and no `both()` projection.
Its `hot()` and `cold()` ranges are explicit projections over one physical side for intentional tier-specific traversal.
`entryCount()` reports complete logical Track rows only and fails through `AO_INVARIANT` if the hot and cold physical row counts diverge.
`TrackStore::Reader::get()` and `visitTracks()` retain `LoadMode` for point and ordered-batch side selection and treat absence as the only normal miss.
A loaded but structurally invalid hot or cold side after successful open fails through `AO_INVARIANT`; it is not returned as a poisoned live Store row.
Directly constructed `TrackView` remains available for bounded binary diagnostics and preserves its independent validity queries.

Mapped Store record views borrow their bytes from the active LMDB transaction.
It must not outlive that transaction.
Within a write transaction, a subsequent database mutation may invalidate an earlier borrowed value even when the C++ `ListView` object is still in scope.
Code that writes after reading a List must first copy every needed scalar, string, rank vector, or serialized payload, or reacquire the List after the intervening write; it must never dereference the old view afterward.

Record validation operates inside the mapped-storage fault-containment limit defined by the [LMDB operation specification](../../../system/persistence/lmdb-operation.md#failure-and-cancellation).
It cannot turn an underlying mapped-file fault into a recoverable record-validation result.

## Compatibility and versioning

Opening a database with invalid metadata magic returns `CorruptData`; a valid non-current stored version returns `NotSupported` after only the stable prefix is read.
There is no migration path today; the current reset-and-rescan recovery instruction loses database-only user-authored state and must be treated as an explicit destructive fallback rather than a reconstruction guarantee.
Safely detected malformed catalog, metadata, dictionary, Resource, Track, List, or manifest state likewise rejects open as a unit.
Preserving curation requires a usable YAML export or another backup made before damage; Aobus does not assume a damaged database can still be exported.
The Track write sequencing, validation, and return-value contracts do not change stored bytes, so they require neither a format-version increment nor a migration.

Version `10` gates the id space record and its never-reused Track and List ids, the explicit zero reserved word in the metadata header, the non-nil library id, and the `UINT32_MAX` List-key reservation.
Only the exact version-10 layout defined here is accepted; alternative layouts labeled version 10 are unsupported.
Version 10 has not been released. Until the first public release, a development build may change its layout or widen one of its limits without incrementing it, and the result is an alternative layout to earlier development builds.
An earlier build rejects such a database on open as `CorruptData`, and one that already has it open fails through the fatal facility when it reads an affected row; for example, a URI longer than 500 bytes is rejected by builds made before the limit became 1,976 bytes.
The [YAML version 8 reference](../format/yaml.md) separately defines the accepted interchange schema. Neither format provides a dual reader or layout autodetection.
Version `9` gated the independent work and performance blocks, partial RecordingDate, and segmented Credits described above.
Version `8` gated the portable manifest modification-time record: the 12-byte `FileTimestamp` seconds/nanoseconds instant with its `hasMtime` absence flag replaces the unsigned host file-clock count.
Version `7` gated scalar-valid UTF-8 NFC admission for dictionary values, inline Track text, and List display text; it deliberately excluded filesystem URI bytes and opaque List filter source.
Version `6` gated the `resources` descriptor record and its reachability rule; version `5` gated the `orderTrackIds` representation and List record layout, while stored `filter` text remains syntactically opaque to database admission.
The current application interpretation belongs to the [predicate language reference](../../query/predicate-language.md), and membership behavior belongs to the [predicate evaluation specification](../../../system/query/predicate-evaluation.md).
The database version also names the predicate language of stored List filters, which carry no version of their own.
A grammar, field-binding, or evaluation change that alters the outcome of filter text the current language accepts therefore increments `kLibraryVersion`, so that a later migration can tell which language a stored filter was written in; a change that only gives meaning to text the language rejects does not, as the [predicate language reference](../../query/predicate-language.md#compatibility-and-versioning) explains.
Admission still never parses a filter, and stored text that does not parse or compile is an application expression error rather than corrupt storage.

Any incompatible key, record, enum encoding, slot meaning, signature algorithm, List byte layout, or saved-order representation change must increment `kLibraryVersion`.
There is no reader for an older physical version, no in-place migration, and no metadata conversion or backfill.
An explicitly tested future migration may replace reset-and-rescan recovery for an old physical version only when it converts or validates every affected record atomically and updates the metadata version after the converted data is valid; no such migration exists today.
A version-9 or earlier environment returns `NotSupported`; opening it never relabels, rewrites, or deletes it automatically.
Explicit recreation by rescanning abandons database-only curation, lists, and other state not held in the media files; it is a destructive user choice, not a reconstruction guarantee.
The [YAML version 8](../format/yaml.md) surface likewise rejects version-7 documents rather than accepting an old-layout subset.
The current build cannot open an old environment to export its curation, and an old-format backup does not become a current-format import merely by changing its version number.
Transaction-local dictionary publication does not change the row shape or library version; it assumes a freshly created index and adds no legacy-layout migration or validation path.

## Implementation authority

- [`MetadataLayout.h`](../../../../include/ao/library/MetadataLayout.h) owns magic, version, metadata and id space records, and the byte-order and word-size guards.
- [`WriteTransaction.cpp`](../../../../lib/library/WriteTransaction.cpp) owns reading the id space at begin and committing its marks; [`MusicLibrary.cpp`](../../../../lib/library/MusicLibrary.cpp) draws the library id when it creates the environment.
- [`TrackLayout.h`](../../../../include/ao/library/TrackLayout.h), [`ListLayout.h`](../../../../include/ao/library/ListLayout.h), and [`FileManifestLayout.h`](../../../../include/ao/library/FileManifestLayout.h) own binary structs and static size checks.
- [`FileTimestamp.h`](../../../../include/ao/FileTimestamp.h) owns the passive `ao::FileTimestamp` value type, while [`FileTimeConversion.h`](../../../../include/ao/library/FileTimeConversion.h) and [`FileTimeConversion.cpp`](../../../../lib/library/FileTimeConversion.cpp) own the native filesystem-clock conversion and last-write-time read.
- [`TrackRecordValidation.cpp`](../../../../lib/library/TrackRecordValidation.cpp), [`ListRecordValidation.cpp`](../../../../lib/library/ListRecordValidation.cpp), [`FileManifestValidation.cpp`](../../../../lib/library/FileManifestValidation.cpp), and [`LibraryUriValidation.h`](../../../../lib/library/LibraryUriValidation.h) own the canonical persisted-record validation implementations.
- [`MusicLibrary.cpp`](../../../../lib/library/MusicLibrary.cpp) owns environment, named-database creation, the private version-before-flags admission sequence, and the complete open gate; [`UnvalidatedDatabase.h`](../../../../lib/lmdb/detail/UnvalidatedDatabase.h) owns its source-private one-DBI read-and-classify capability, while [`DictionaryStore.cpp`](../../../../lib/library/DictionaryStore.cpp) establishes the dictionary representation while loading it.
- [`OpenValidationMetrics.cpp`](../../../../lib/library/OpenValidationMetrics.cpp)
  is the source-private operation-count probe used only to lock the one-open-per-named-DBI rule and open gate's
  linear Track/manifest growth law; the library build guard limits recording
  and reset to the open owner and prevents other production consumers.
- [`ReadTransaction.h`](../../../../include/ao/library/ReadTransaction.h) and [`WriteTransaction.h`](../../../../include/ao/library/WriteTransaction.h) own the public transaction capabilities; [`LibraryWrite.h`](../../../../include/ao/library/LibraryWrite.h) owns the callback-scoped mutation capability.
- [`TrackWriter.h`](../../../../include/ao/library/TrackWriter.h), [`TrackWriter.cpp`](../../../../lib/library/TrackWriter.cpp), [`ListWriter.h`](../../../../include/ao/library/ListWriter.h), and [`ListWriter.cpp`](../../../../lib/library/ListWriter.cpp) own logical cross-Store and cross-row mutation invariants.
- [`TrackWrite.cpp`](../../../../lib/library/TrackWrite.cpp) and [`ReservationWriterAccess.h`](../../../../lib/lmdb/detail/ReservationWriterAccess.h) own the source-private path from prepared hot/cold Track values to callback-scoped integer-key reservations.
- Store and builder implementations under [`lib/library/`](../../../../lib/library) own key allocation, private preparation, and physical write validation.
- [`lib/library/CMakeLists.txt`](../../../../lib/library/CMakeLists.txt) enforces that production code cannot include, define, or invoke the source-private physical access seam or call internal Track encoder helpers.

## Test authority

- [`MusicLibraryTest.cpp`](../../../../test/unit/library/MusicLibraryTest.cpp) covers exact catalog/header/revision/id space admission, non-current-version precedence over current key flags, one-open-per-named-DBI behavior, dictionary/Resource/Track/List/manifest closure, accepted opaque and stale states, recoverable validation-read faults, deterministic `N`/`2N` operation counts, and same-library plus cross-process writer-session exclusion.
- [`FileTimeConversionTest.cpp`](../../../../test/unit/library/FileTimeConversionTest.cpp) locks the native file-clock conversion behind the manifest modification-time record: Unix seconds with a nonnegative fraction, epoch zero, instants beyond the Unix nanosecond range, leap-second mapping, and `FileTimestamp` ordering and normalization.
- [`IdSpaceTest.cpp`](../../../../test/unit/library/IdSpaceTest.cpp) covers never-reused Track and List ids across deletion, clear, and reopen, the library id across clear, failed-commit rollback, the saved-order mark, and the List id ceiling.
- [`MetadataStoreTest.cpp`](../../../../test/unit/library/MetadataStoreTest.cpp) covers logical metadata snapshots, failed-commit rollback, and durable candidate-revision sequencing across a child-process commit.
- [`LibraryUriTest.cpp`](../../../../test/unit/library/LibraryUriTest.cpp) locks parsing and the allocation-free persisted canonical predicate to the same canonical spelling.
- [`ListLayoutTest.cpp`](../../../../test/unit/library/ListLayoutTest.cpp), [`ListBuilderTest.cpp`](../../../../test/unit/library/ListBuilderTest.cpp), and [`ListViewTest.cpp`](../../../../test/unit/library/ListViewTest.cpp) lock the 20-byte header, field offsets, canonical packing, checked sizing, opaque filter bytes, prepared snapshots, and padding gate.
- [`ListStoreTest.cpp`](../../../../test/unit/library/ListStoreTest.cpp) locks the prepared-only writer surface, pre-mutation validation, and post-open fail-fast point-read, writer-read, and iteration behavior.
- [`FileManifestBuilderTest.cpp`](../../../../test/unit/library/FileManifestBuilderTest.cpp) and [`FileManifestStoreTest.cpp`](../../../../test/unit/library/FileManifestStoreTest.cpp) lock unbound and bound snapshots, binding-independent validation, the prepared-only writer surface, manifest validation, URI padding boundaries through the maximum key size, point-read and lower-bound outcomes, and post-open iterator fail-fast behavior.
- [`LibraryProbeTest.cpp`](../../../../test/unit/library/LibraryProbeTest.cpp) locks invalid-view and prepared-write contracts, post-open structural, List-parent, Track/manifest, and native-read failures, revision exhaustion, LMDB lifetime misuse, empty lower-bound rejection, fresh-environment reader-table exhaustion, the default request exceeding LMDB's 126-slot baseline through 160 simultaneous snapshots, and bounded normal child-process observations.
- [`RuntimeFatalProbeTest.cpp`](../../../../test/unit/runtime/library/RuntimeFatalProbeTest.cpp) locks a runtime YAML consumer's post-open Resource-reference invariant to the same fatal diagnostics.
- [`PerformanceBaselineTest.cpp`](../../../../test/perf/PerformanceBaselineTest.cpp) records the non-default 100,000-Track open-admission wall-time, sampled resident-memory, named-DBI-open, cursor-row, and manifest-point-read evidence.
- [`TrackWriterTest.cpp`](../../../../test/unit/library/TrackWriterTest.cpp) locks the public/physical capability boundary and coherent Track, manifest, dictionary, Resource, update, relink, delete, and clear behavior, including a maximum-length URI across reopen.
- [`ListWriterTest.cpp`](../../../../test/unit/library/ListWriterTest.cpp) locks live parent validation, leaf-delete conflict, children-first subtree deletion, and coherent clear behavior.
- [`TrackStoreRawLayoutTest.cpp`](../../../../test/unit/library/TrackStoreRawLayoutTest.cpp) locks record layout, the complete Reader range and count surface, physical-side projections, retained point and batch load modes, and ordinary store behavior.
- [`TrackStoreIntegrityTest.cpp`](../../../../test/unit/library/TrackStoreIntegrityTest.cpp) locks reserved-id rejection and the canonical sweep over persisted records.
- [`DatabaseWriterTest.cpp`](../../../../test/unit/lmdb/DatabaseWriterTest.cpp) locks the copied-data-only public LMDB writer surface and the source-private reservation encoder constraints and failure paths.
- [`TrackPerformanceTest.cpp`](../../../../test/unit/library/TrackPerformanceTest.cpp) specifies independent block omission, ordered admission, date precision, complete reconstruction, hot-only preservation, bounds-before-write, and owning prepared snapshots.
- [`TrackPerformanceValidationTest.cpp`](../../../../test/unit/library/TrackPerformanceValidationTest.cpp) specifies malformed performance and referenced-text rejection, including unchanged stored cold rows after rejected open.
- [`TrackBuilderOverflowTest.cpp`](../../../../test/unit/library/TrackBuilderOverflowTest.cpp) locks the cold-record limit: the largest record is written and admitted, one more byte is rejected, and each rejection names the component that crosses the limit.
- Other layout and serialization tests under [`test/unit/library/`](../../../../test/unit/library) lock the remaining record sizes, alignment, validation, and store behavior.

## Related documents

- [Resource descriptors](../../resource/blob.md)
- [Library architecture](../../../system/library/structure.md)
- [LMDB operation specification](../../../system/persistence/lmdb-operation.md)
- [Library access and mutation](../../../system/library/mutation.md)
- [Library scan and audio identity](../../../system/library/scan-and-identity.md)
- [List model](../model/list.md)
- [Predicate language](../../query/predicate-language.md)
- [Persistence and managed-state architecture](../../../system/persistence/README.md)
