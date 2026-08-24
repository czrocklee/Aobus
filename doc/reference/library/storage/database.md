---
id: library.database
type: reference
status: current
domain: library
summary: Defines the version 7 host-local LMDB environment, named databases, keys, records, and validation gates.
---
# Library database

## Scope and version

This reference defines physical library format version `7`, gated by `ao::library::kLibraryVersion`.
It owns the LMDB environment, named databases, key encodings, record composition, size and alignment requirements, and version policy.

Entity meaning belongs to the [track](../model/track.md) and [list](../model/list.md) references.
Scan and identity behavior belongs to the [scan and audio identity specification](../../../spec/library/runtime/scan-and-identity.md).

## Code boundary

This surface belongs to the **core libraries** layer in the [system architecture](../../../architecture/system-overview.md).
`ao::library::MusicLibrary`, its stores, builders, views, and LMDB adapter dependencies live under `include/ao/library/` and `lib/library/`; application-runtime commands may coordinate them but do not own or redefine this format.

## Environment

The library is one LMDB environment at the database path passed to `MusicLibrary::open`; normal application composition supplies `<music-root>/.aobus/library`.
Application composition keeps only one live environment for a database path in each process, as required by LMDB; `MusicLibrary` has no canonical-path registry or duplicate-open admission mechanism.
Independent processes may open the same environment under LMDB's normal locking rules.
`MusicLibrary::open` uses `MDB_NOTLS`, allows eight named databases, and configures the LMDB reader table through `MusicLibrary::Options::maxReaders`.
That option is a requested table size, defaults to `512`, and skips `mdb_env_set_maxreaders` when zero.
The effective capacity comes from the persistent LMDB lock file: an opener that acquires the exclusive environment lock may grow a smaller table to the requested size, a concurrent opener adopts the existing size, and no opener shrinks a larger table.
Each live `MusicLibrary::readTransaction()` snapshot occupies one effective slot until destruction under `MDB_NOTLS`.
Exhausting that table follows the existing fatal transaction-begin contract owned by the [library architecture](../../../architecture/library.md#failure-cancellation-and-lifetime-boundaries); `maxReaders` raises capacity but is not a concurrency throttle.
That map is the capacity this environment may grow into, not disk it occupies: the [LMDB adapter](../../../spec/storage/lmdb-operation.md) prepares the data file so allocation follows committed use, and a full map remains the point at which mutation fails with `StorageFull`.
`MusicLibrary::storageCapacity()` reports that capacity as `mapBytes` together with the `highWaterBytes` the database has needed; the high water is the peak page extent rather than live data, so deleting rows does not lower it.

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
It first requires byte-key flags for the main LMDB database, then enumerates that catalog before any named database is created and initializes the exact version-7 schema only when that catalog is empty.
Fresh and existing admission open all seven named DBIs sequentially and exactly once in that initialization write transaction.
Existing admission opens `meta` into one source-private, read-only unvalidated token, reads the stable version prefix, and—only for the current version—consumes that same DBI into an `IntegerKeyDatabase` after exact flag validation; it never reopens `meta`.
The resulting integer-key and byte-key tokens remain internal to the library and are reused by later read and write transactions.
A nonempty environment must contain the existing `meta` database and metadata header; a partial schema or ordinary main-database record is `CorruptData`, not a partially initialized new library.
After the version gate accepts version 7, open requires exactly the seven named databases below, their exact key flags, the two allowed metadata records, and every local and cross-Store invariant described below before exposing any store.

The database is host-local rather than an interchange format.
It combines regenerable scan facts with user-authored lists, membership, curated metadata, tags, covers, custom metadata, and stable library/track identities; the complete environment is therefore not rebuildable from media files.
Its path must reside on a filesystem local to the host. Network filesystems and
shares are unsupported because neither the writer lease nor LMDB's mapped-file
locking provides cross-host safety for this database.
Integer keys use LMDB native word order and record structs are host-endian; [library YAML](../format/yaml.md) is the portable interchange surface.

## Transaction access

`MusicLibrary::readTransaction()` returns a move-only `ReadTransaction` that directly owns one native LMDB read snapshot plus the header and revision values read from that snapshot.
Failure to begin the native transaction is fatal because a live library has no recoverable storage-snapshot alternative.
`WritableMusicLibrary::acquire(MusicLibrary&)` returns the explicit move-only capability whose `writeTransaction()` factory returns a `WriteTransaction`; `MusicLibrary` exposes no public write-transaction factory.
After native begin has acquired LMDB's single-writer snapshot, the factory reads that snapshot's durable header and revision and computes the transaction's one in-memory candidate revision before exposing the wrapper.
Either failure releases the process writer gate and discards the transaction's lease-anchor reference, then fails fatally instead of creating a recoverable authoring result.
The originating writable capability continues to hold its process session lease.
The write wrapper owns one native transaction, the transaction-local dictionary writer hidden behind Track preparation, the process writer gate, a shared anchor to the writable capability's lease, and one lazily opened physical writer per touched Store.
Repeated logical operations and successive successful `apply()` callbacks reuse those writers and their native cursors until commit or abort.
It exposes logical Track, List, and identity mutation authority only through the callback-scoped `LibraryWrite` supplied by `WriteTransaction::apply()`.

The specialized stores are const read service handles.
Their readers accept a read transaction, a write transaction for pre-operation inspection, or the active `LibraryWrite` context for a coherent in-operation snapshot.
Production mutation does not obtain a physical Store writer: `LibraryWrite::tracks()` and `LibraryWrite::lists()` return callback-scoped logical writers, and `LibraryWrite::restoreLibraryIdentity()` is the only metadata mutation exposed inside the root operation.
Physical Track, manifest, List, Resource, Dictionary, and metadata writer factories remain inaccessible to production callers; representation, corruption, and isolated Store-backed tests use one source-private access seam.
`MusicLibrary` exposes logical metadata-header values rather than a physical Metadata Store handle.
Every overload that reads a transaction-local metadata header or library revision validates the transaction's stable `MusicLibrary` identity before returning the fact; cross-library fact reads fail before exposing either value.
Native LMDB transaction handles remain private implementation details of `MusicLibrary` and the stores; the wrappers add semantic capability boundaries but no additional storage transaction or heap allocation on the read path.

The logical Track writer has these operation groups:

- validate a `TrackBuilder` for representability and existing Resource references without writing dictionary or Resource rows;
- create one hot/cold Track pair together with its manifest row;
- update complete, hot-only, or cold-only Track data while retaining its existing URI;
- replace Track data and its existing manifest facts together;
- update only file status or audio identity while retaining the existing URI-to-Track binding;
- relink by changing the cold URI, removing the old manifest key, and creating the new binding as one operation;
- delete one Track together with its manifest row; and
- clear Tracks and the manifest together.

The logical List writer accepts `ListBuilder`, prepares its physical row internally, and creates or updates it only after validating the live parent chain.
Update first proves that its target exists, so the physical LMDB upsert primitive cannot create a caller-selected List id.
An ordinary delete returns `NotFound` for an absent id and `Conflict` while a child exists.
Explicit subtree deletion discovers the complete live subtree, deletes children before parents, and returns the deleted ids in root-first discovery order.
Clear removes the complete List graph coherently.
Filter bytes remain opaque throughout these storage operations.

Writable-capability acquisition non-blockingly locks `<database-path>/.aobus-writer.lock` for the capability lifetime.
An active write transaction retains the lock after its originating capability is destroyed and releases it on commit, failure, abort-by-destruction, or transaction destruction.
The lock file has no governed payload and is not part of format version `7`, but it must not be removed while a writable process is active.

## Named databases

| Database | Key | Value |
|---|---|---|
| `meta` | Fixed integer record id | Record `1`: `MetadataHeader`; record `2`: one `std::uint64_t` library revision. |
| `tracks_hot` | `TrackId` integer | `TrackHotHeader`, tag-id array, title bytes. |
| `tracks_cold` | `TrackId` integer | `TrackColdHeader`, optional block payloads, URI bytes. |
| `lists` | `ListId` integer | `ListHeader`, order-track-id array, name/description/filter bytes, zero padding. |
| `resources` | Digest-derived `ResourceId` integer | 36-byte `ResourceDescriptor`. |
| `dictionary` | `DictionaryId` integer | Scalar-valid UTF-8 NFC bytes without a terminator. |
| `file_manifest` | Root-relative URI padded to a four-byte multiple | `FileManifestHeader`. |

One database slot remains spare.

## Keys and identifiers

All integer identifiers are 32-bit values and reserve `0` as invalid.

- Track and list writers allocate `maxKey + 1`; the first id is `1`, and exhaustion returns `ResourceExhausted`.
- A track is appended to `tracks_hot` first and written to `tracks_cold` under the same id.
- A resource key starts from the first four bytes of the content's SHA-256 digest read big-endian, remaps zero to one, and linearly probes with digest comparison, stopping at the first empty slot; identical content reuses the existing id.
- Dictionary ids produced by current writers are the dense committed range `1..entryCount`; input text is normalized to NFC before identity lookup, new canonical text receives the next id, canonically equivalent text reuses its existing id, and committed ids are never deleted or rebound.
- Persisted dictionary, Track, and List integer keys are exactly four native bytes before conversion; dictionary keys are the dense range `1..N`, Track keys are nonzero matching hot/cold pairs, and List keys are nonzero.
- An aborted transaction-local dictionary tail has no durable identity and its ids may be reused by a later transaction.
- The dictionary persists id-to-string rows and rebuilds its string-to-id index in memory when opened; that same traversal validates key width, dense order, scalar-valid UTF-8 NFC text, and canonical text uniqueness before the store is exposed.
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
Absence means the initial committed revision `0`; a present record must be exactly eight bytes and contain neither `0` nor `UINT64_MAX`.
The maximum valid committed value is `UINT64_MAX - 1`, a physically unreachable exhaustion sentinel for this desktop application's mutation rate.
A write transaction computes the next value in memory from the durable revision visible to its already-acquired LMDB writer snapshot, exposes it as its candidate revision, and persists it in that same native transaction immediately before commit.
Abort, pre-commit rejection, and failed commit leave the previous durable revision unchanged.

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
Inline title and custom-metadata value bytes are scalar-valid UTF-8 in NFC.
Every dictionary-backed metadata field, tag, and custom-metadata key inherits the same text invariant from its dictionary row.
The URI is filesystem identity and is explicitly outside this text-normalization contract.

Production callers pass a `TrackBuilder` to the logical writer returned by `LibraryWrite::tracks()`.
The writer performs preparation itself and keeps the resulting immutable `TrackBuilder::PreparedHot` and `PreparedCold` values inside `ao_library`.
This binds resolved dictionary ids and Resource descriptor creation, from hashed bytes or from a declared descriptor, to the same library and transaction that performs the physical write; caller-supplied existing Resource ids must already exist in that write snapshot.
The physical `TrackStore::Writer` accepts no caller-supplied record bytes and exposes no mutable LMDB span, and it is unavailable to production callers, so there is one record-writing path rather than a prepared path beside a raw one.
Public LMDB create, update, and append operations accept only copied input bytes.
The Track writer is the only production consumer that reaches the integer writer's private reservation primitives through source-private `lmdb::detail::ReservationWriterAccess`; its synchronous no-throw encoder receives the four-byte-aligned reservation only for that invocation, and the span is never returned to the Store.
Writes reject the reserved zero `TrackId` as a `CorruptData` fault.
Complete internal preparation first runs both pure hot and cold validators; single-side logical updates run the validator for that side.
Private serialization helpers used by representation tests follow the same preflight rules.
Those gates validate scalar UTF-8 and check every representable post-NFC size plus the canonical Track URI before dictionary interning, resource creation, or Track mutation begins, so a recoverable rejection adds no item-relative dictionary, resource, or Track delta.
Already-NFC input uses a quick check to avoid a temporary normalized allocation during size preflight; prepared text is normalized once before it is encoded or interned.
They validate builder input and representability rather than encoded bytes: a prepared Track side is a typed zero-copy snapshot and does not retain a second serialized record.
That private zero-copy path reserves the hot value, fills every byte, and validates it before its encoder callback returns, then repeats the sequence for the cold value.
Updates follow the same per-side sequence.
Prepared writes fill and validate transaction-owned storage entirely inside the synchronous encoder invocation.
A canonical post-fill validation failure violates the encoder's `AO_ENSURES` postcondition and aborts immediately.

Create requires both canonical payloads and allocates the identity by appending to `tracks_hot`, then creates the same key in `tracks_cold`.
After open proves matching key sets, a cold-key conflict after hot allocation violates the
Track-pair invariant and aborts; supported writers cannot create that state. Other native
failure after the hot reservation uses the private transaction carrier so the root aborts the
whole write before returning its typed storage error.
After open establishes the exact-pair invariant, update helpers do not rescan the opposite database as a recovery check.
A `Both` point lookup still probes both sides so it can distinguish a normal miss, where neither row exists, from a post-open invariant breach, where exactly one row exists.
A single-side update checks its target row and returns `NotFound` without terminating the transaction when that row is absent.
The private complete-update encoder checks the hot target once before its first mutation, then relies on the established pair invariant for the cold replacement.
It changes both sides as one logical operation.
If any multi-side mutation reaches its first successful storage update and a later storage step fails, the mutation raises `lmdb::detail::TransactionFailure`; the root operation owner explicitly aborts before translating the failure to its enclosing `Result` boundary.

Expected no-mutation outcomes (`NotFound`, `Conflict` from an exclusive create, and integer-key exhaustion) are returned as `Result` values and leave the lower store transaction mechanically usable.
When such an error is the result of a root `WriteTransaction::apply()` body, root policy still aborts the complete transaction before returning it.
An LMDB fault from a mutation path is carried by `lmdb::detail::TransactionFailure` because continuing or committing could expose only a successful prefix of the logical operation.
`WriteTransaction::apply()` catches that marker, explicitly aborts every uncommitted row, terminalizes the wrapper, and returns the carried error.
`WriteTransaction::commit()` provides the same containment for a mutation fault during dictionary preparation.
No public runtime writer exposes a transaction that may be continued after either failure.

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
The row describes content and holds none of it; the content lives in the media files the library indexes, and the [cover-art delivery specification](../../../spec/resource/cover-art-delivery.md) owns how it is materialized.
The `resources` database is append-only in practice: no production path deletes a row, because `create` stops probing at the first empty slot and a hole in a collision chain would let a later create mint a second row for one digest.
Rows a track no longer references are retained and remain valid.

Dictionary values are scalar-valid UTF-8 NFC bytes with no header or terminator.
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

Manifest point reads, lower-bound seeks, iteration, and writes share one exact record validator.
Keys must be nonempty canonical `LibraryUri` bytes with the minimal zero padding needed to reach a four-byte multiple.
Values must be exactly 48 bytes, carry a nonzero Track id, a declared status, three zero reserved bytes, and either both parts of an audio identity or neither.
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
A valid non-current version returns `NotSupported` before version-7 catalog closure, exact header size, named-database flags, or extra-database checks; migration remains a separate facility even when the old named database's key flags differ from the current schema.
The main database's byte-key flags are admitted before safe catalog enumeration and therefore before the metadata version lookup.
For version 7, the header is exactly 40 bytes with zero flags and the catalog is exactly the seven named databases above.
Admission then requires exact `MDB_INTEGERKEY` flags before consuming the existing `meta` DBI as an `IntegerKeyDatabase`, opens the remaining integer-key databases as `IntegerKeyDatabase`, opens `file_manifest` as `ByteKeyDatabase` with no key flags, and permits no unvalidated token to escape initialization.
The typed `meta` token contains only header record `1` plus optional revision record `2`.

The current-schema gate then validates Resource keys as nonzero four-byte ids and Resource values as exactly 36 parseable descriptor bytes, requires every stored digest to be distinct, and requires every row to be reachable from its digest's initial key along an unbroken run of occupied slots; orphan Resources and orphan dictionary rows are accepted.
A row whose key is not the first free slot at or above its digest's initial key is `CorruptData`, with the empty slot before it as the cause; a long collision cluster and a cluster spanning the wrap from the maximum key to `1` are both valid.
Reachability is checked over the occupied key runs the same traversal already collects, so it stays linear in the number of rows.
It validates dictionary key width, dense ids, scalar-valid UTF-8 NFC, and unique canonical text while building the in-memory index.
It merge-checks the hot/cold Track key sets, validates canonical records including scalar-valid UTF-8 NFC inline text plus every dictionary and Resource reference, and proves a strict Track-to-manifest bijection.
The bijection first compares row counts, then performs one canonical manifest point read for each Track URI and requires the manifest's Track id to equal that Track id.
Equal counts, unique Track ids, and the exact point matches prove that no extra manifest row exists without allocating a Track- or manifest-sized set; duplicate Track URIs and duplicate manifest bindings cannot pass.
It validates every List key and local record, including scalar-valid UTF-8 NFC text, requires each non-root parent to exist, and rejects parent cycles using memory proportional to the List count.
Saved-order Track ids must be nonzero and unique, but they may be stale or currently absent because saved rank is intentionally retained outside current membership.
Stored filter bytes remain opaque and are not parsed or compiled by database admission.
These gates all complete in one coherent initialization transaction before exposure; their internal evaluation order after the version gate is not an error-precedence contract.
The first observed failure returns `CorruptData` for the complete open.
No `MusicLibrary`, runtime source, partial All Tracks membership, or salvage-row view is exposed.
The admission algorithm is linear in the number and total byte size of persisted rows.
Track-to-manifest closure adds one manifest point lookup per Track and constant Track-sized auxiliary storage; List topology adds linear auxiliary storage in the number of Lists.
Tests lock the operation-count slope from `N` to `2N`, and a manual 100,000-Track evidence run records wall time and peak resident memory without imposing a machine-dependent CI time threshold.

Directly constructed read views perform one constant-time structural gate that proves the fixed header and all derived slices remain inside the record.
`TrackView` gates its hot and cold sides independently because callers may deliberately load only one side.
`isHotValid()` and `isColdValid()` are always legal and report whether the corresponding loaded side passed its gate.
A decoded hot or cold accessor requires that side to be valid; calling it for an absent or structurally invalid side is a programmer error that fails fast through `AO_EXPECTS`.
Raw diagnostic access remains available for the exact bytes supplied to the view.
An absent optional block inside a valid cold side is not an invalid tier: classical, cover-art, and custom-metadata proxies remain legal and empty, with their documented optional-block defaults.
The canonical write validator additionally checks exact size and zero padding, tag ids and bloom agreement, and cold block ordering.
Those linear checks do not add a per-row scan to normal decoded access.
`TrackBuilder::fromCompleteView()` requires both valid sides.
`fromHotView()` accepts a valid hot-only view but the resulting builder may serialize only hot data, preventing absent cold fields from being written back as defaults.

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

Every store view borrows its bytes from the active LMDB transaction.
It must not outlive that transaction.
Within a write transaction, a subsequent database mutation may invalidate an earlier borrowed value even when the C++ `ListView` object is still in scope.
Code that writes after reading a List must first copy every needed scalar, string, rank vector, or serialized payload, or reacquire the List after the intervening write; it must never dereference the old view afterward.

Record validation operates inside the mapped-storage fault-containment limit defined by the [LMDB operation specification](../../../spec/storage/lmdb-operation.md#failure-and-cancellation).
It cannot turn an underlying mapped-file fault into a recoverable record-validation result.

## Compatibility and versioning

Opening a database with invalid metadata magic returns `CorruptData`; a valid non-current stored version returns `NotSupported` after only the stable prefix is read.
There is no migration path today; the current reset-and-rescan recovery instruction loses database-only user-authored state and must be treated as an explicit destructive fallback rather than a reconstruction guarantee.
Safely detected malformed catalog, metadata, dictionary, Resource, Track, List, or manifest state likewise rejects open as a unit.
Preserving curation requires a usable YAML export or another backup made before damage; Aobus does not assume a damaged database can still be exported.
The Track write sequencing, validation, and return-value contracts do not change stored bytes, so they require neither a format-version increment nor a migration.

Version `7` gates scalar-valid UTF-8 NFC admission for dictionary values, inline Track text, and List display text; it deliberately excludes filesystem URI bytes and opaque List filter source.
Version `6` gated the `resources` descriptor record and its reachability rule; version `5` gated the `orderTrackIds` representation and List record layout, while stored `filter` text remains syntactically opaque to database admission.
The current application interpretation belongs to the [predicate language reference](../../query/predicate-language.md), and membership behavior belongs to the [predicate evaluation specification](../../../spec/query/predicate-evaluation.md).
A grammar or predicate-semantic change does not by itself increment `kLibraryVersion`; stored text that no longer parses or compiles is an application expression error rather than corrupt storage.

Any incompatible key, record, enum encoding, slot meaning, signature algorithm, List byte layout, or saved-order representation change must increment `kLibraryVersion`.
An explicitly tested future migration may replace reset-and-rescan recovery for an old physical version only when it converts or validates every affected record atomically and updates the metadata version after the converted data is valid; no such migration exists today.
There is no reader for an older physical version and no in-place migration.
Opening a version-4, version-5, or version-6 environment returns `NotSupported`; recreating the library by rescanning the music root is the supported answer.
Preserving user-authored curation instead requires a current version-5 portable export made before the physical upgrade.
Transaction-local dictionary publication does not change the row shape or library version; it assumes a freshly created host-local index and adds no legacy-layout migration or validation path.

## Implementation authority

- [`MetadataLayout.h`](../../../../include/ao/library/MetadataLayout.h) owns magic, version, and metadata sizes.
- [`TrackLayout.h`](../../../../include/ao/library/TrackLayout.h), [`ListLayout.h`](../../../../include/ao/library/ListLayout.h), and [`FileManifestLayout.h`](../../../../include/ao/library/FileManifestLayout.h) own binary structs and static size checks.
- [`TrackRecordValidation.cpp`](../../../../lib/library/TrackRecordValidation.cpp), [`ListRecordValidation.cpp`](../../../../lib/library/ListRecordValidation.cpp), [`FileManifestValidation.cpp`](../../../../lib/library/FileManifestValidation.cpp), and [`LibraryUriValidation.h`](../../../../lib/library/LibraryUriValidation.h) own the canonical persisted-record validation implementations.
- [`MusicLibrary.cpp`](../../../../lib/library/MusicLibrary.cpp) owns environment, named-database creation, the private version-before-flags admission sequence, and the complete open gate; [`UnvalidatedDatabase.h`](../../../../lib/lmdb/detail/UnvalidatedDatabase.h) owns its source-private one-DBI read-and-classify capability, while [`DictionaryStore.cpp`](../../../../lib/library/DictionaryStore.cpp) establishes the dictionary representation while loading it.
- [`OpenValidationMetrics.cpp`](../../../../lib/library/OpenValidationMetrics.cpp)
  is the source-private operation-count probe used only to lock the one-open-per-named-DBI rule and open gate's
  linear Track/manifest growth law; the library build guard limits recording
  and reset to the open owner and prevents other production consumers.
- [`ReadTransaction.h`](../../../../include/ao/library/ReadTransaction.h) and [`WriteTransaction.h`](../../../../include/ao/library/WriteTransaction.h) own the public transaction capabilities; [`LibraryWrite.h`](../../../../include/ao/library/LibraryWrite.h) owns the callback-scoped mutation capability.
- [`TrackWriter.h`](../../../../include/ao/library/TrackWriter.h), [`TrackWriter.cpp`](../../../../lib/library/TrackWriter.cpp), [`ListWriter.h`](../../../../include/ao/library/ListWriter.h), and [`ListWriter.cpp`](../../../../lib/library/ListWriter.cpp) own logical cross-Store and cross-row mutation invariants.
- [`TrackWrite.cpp`](../../../../lib/library/TrackWrite.cpp) and [`ReservationWriterAccess.h`](../../../../lib/lmdb/detail/ReservationWriterAccess.h) own the source-private path from prepared hot/cold Track values to callback-scoped integer-key reservations.
- Store and builder implementations under [`lib/library/`](../../../../lib/library/) own key allocation, private preparation, and physical write validation.
- [`lib/library/CMakeLists.txt`](../../../../lib/library/CMakeLists.txt) enforces that production code cannot include, define, or invoke the source-private physical access seam or call internal Track encoder helpers.

## Test authority

- [`MusicLibraryTest.cpp`](../../../../test/unit/library/MusicLibraryTest.cpp) covers exact catalog/header/revision admission, non-current-version precedence over current key flags, one-open-per-named-DBI behavior, dictionary/Resource/Track/List/manifest closure, accepted opaque and stale states, recoverable validation-read faults, deterministic `N`/`2N` operation counts, and same-library plus cross-process writer-session exclusion.
- [`MetadataStoreTest.cpp`](../../../../test/unit/library/MetadataStoreTest.cpp) covers logical metadata snapshots, identity publication, failed-commit rollback, and durable candidate-revision sequencing across a child-process commit.
- [`LibraryUriTest.cpp`](../../../../test/unit/library/LibraryUriTest.cpp) locks parsing and the allocation-free persisted canonical predicate to the same canonical spelling.
- [`ListLayoutTest.cpp`](../../../../test/unit/library/ListLayoutTest.cpp), [`ListBuilderTest.cpp`](../../../../test/unit/library/ListBuilderTest.cpp), and [`ListViewTest.cpp`](../../../../test/unit/library/ListViewTest.cpp) lock the 20-byte header, field offsets, canonical packing, checked sizing, opaque filter bytes, prepared snapshots, and padding gate.
- [`ListStoreTest.cpp`](../../../../test/unit/library/ListStoreTest.cpp) locks the prepared-only writer surface, pre-mutation validation, and post-open fail-fast point-read, writer-read, and iteration behavior.
- [`FileManifestBuilderTest.cpp`](../../../../test/unit/library/FileManifestBuilderTest.cpp) and [`FileManifestStoreTest.cpp`](../../../../test/unit/library/FileManifestStoreTest.cpp) lock unbound and bound snapshots, binding-independent validation, the prepared-only writer surface, manifest validation, point-read and lower-bound outcomes, and post-open iterator fail-fast behavior.
- [`LibraryProbeTest.cpp`](../../../../test/unit/library/LibraryProbeTest.cpp) locks invalid-view and prepared-write contracts, post-open structural, List-parent, Track/manifest, and native-read failures, revision exhaustion, LMDB lifetime misuse, empty lower-bound rejection, fresh-environment reader-table exhaustion, the default request exceeding LMDB's 126-slot baseline through 160 simultaneous snapshots, and bounded normal child-process observations.
- [`RuntimeFatalProbeTest.cpp`](../../../../test/unit/runtime/library/RuntimeFatalProbeTest.cpp) locks a runtime YAML consumer's post-open Resource-reference invariant to the same fatal diagnostics.
- [`PerformanceBaselineTest.cpp`](../../../../test/perf/PerformanceBaselineTest.cpp) records the non-default 100,000-Track open-admission wall-time, sampled resident-memory, named-DBI-open, cursor-row, and manifest-point-read evidence.
- [`TrackWriterTest.cpp`](../../../../test/unit/library/TrackWriterTest.cpp) locks the public/physical capability boundary and coherent Track, manifest, dictionary, Resource, update, relink, delete, and clear behavior.
- [`ListWriterTest.cpp`](../../../../test/unit/library/ListWriterTest.cpp) locks live parent validation, leaf-delete conflict, children-first subtree deletion, and coherent clear behavior.
- [`TrackStoreRawLayoutTest.cpp`](../../../../test/unit/library/TrackStoreRawLayoutTest.cpp) locks record layout, the complete Reader range and count surface, physical-side projections, retained point and batch load modes, and ordinary store behavior.
- [`TrackStoreIntegrityTest.cpp`](../../../../test/unit/library/TrackStoreIntegrityTest.cpp) locks reserved-id rejection and the canonical sweep over persisted records.
- [`DatabaseWriterTest.cpp`](../../../../test/unit/lmdb/DatabaseWriterTest.cpp) locks the copied-data-only public LMDB writer surface and the source-private reservation encoder constraints and failure paths.
- Other layout and serialization tests under [`test/unit/library/`](../../../../test/unit/library/) lock the remaining record sizes, alignment, validation, and store behavior.

## Related documents

- [Resource descriptors](../../resource/blob.md)

- [Library architecture](../../../architecture/library.md)
- [LMDB operation specification](../../../spec/storage/lmdb-operation.md)
- [Library access and mutation](../../../spec/library/runtime/mutation.md)
- [Library scan and audio identity](../../../spec/library/runtime/scan-and-identity.md)
- [List model](../model/list.md)
- [Predicate language](../../query/predicate-language.md)
- [Persistence and managed-state architecture](../../../architecture/persistence-and-managed-state.md)
