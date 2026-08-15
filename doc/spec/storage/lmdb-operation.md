---
id: storage.lmdb-operation
type: spec
status: current
domain: storage
summary: Defines LMDB environment, transaction, database, cursor, read, write, and operational failure behavior in the core storage adapter.
---
# LMDB operation specification

## Scope

This specification owns the observable behavior of the core `ao::lmdb` adapter: environment opening, transaction lifetime, named-database access, transaction-scoped reads and writes, iteration, integer-key allocation, commit and abort, and operational failure channels.

It does not define any Aobus library database name, record layout, application mutation, revision event, or runtime task policy.
Concrete music-library keys, records, alignment, and versioning belong to the [library database reference](../../reference/library/storage/database.md); shared failure-channel selection belongs to the [outcome channel specification](../failure/outcome-channel.md); application-level ownership belongs to the [persistence and managed-state](../../architecture/persistence-and-managed-state.md) and [library](../../architecture/library.md) architectures.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places the LMDB adapter in the core-library layer.
Its public API is under `include/ao/lmdb/`, its implementation is under `lib/lmdb/`, and the `ao_lmdb` target depends on the external LMDB library.
The private mutation-failure carrier, library metadata-admission token, and Track-value reservation access are implemented under `lib/lmdb/detail/` and are reachable by `ao_library` only through its private source include path.
A build guard further restricts the admission token to `MusicLibrary` initialization; the reservation access has one production consumer in the library Track encoder.
The `ao_library` core target may otherwise depend on the public `ao_lmdb` surface; the adapter does not depend on library stores, application runtime, UIModel, or frontends.

Runtime, UIModel, and normal frontend public boundaries do not expose LMDB environments, transactions, cursors, database handles, or transaction-scoped byte spans.

## Terminology

- An **environment** owns one native LMDB environment handle.
- **Database-open admission** is the process-wide serialization held from a write transaction's first `mdb_dbi_open` call until that transaction commits or aborts.
- A **read transaction** owns one read snapshot until destruction.
- A **write transaction** owns staged mutations until commit or destruction and also provides read capability.
- An **integer-key database** is an `IntegerKeyDatabase` token whose public reader and writer accept only `std::uint32_t` keys and whose writer owns append allocation.
- A **byte-key database** is a `ByteKeyDatabase` token whose public reader and writer accept only byte-span keys.
- A **reader** borrows a transaction and returns byte views into that transaction's snapshot.
- A **writer** borrows an active write transaction and owns a cursor for its database.
- **End** is the normal cursor state represented by `MDB_NOTFOUND` during iteration.
- An **operational fault** is an LMDB failure other than the normal absence/end cases declared by a particular operation.

## Invariants

- Environments, transactions, iterators, writers, and their native handles follow RAII ownership and are movable but not copyable unless their public type explicitly provides copying.
- `Environment` is move-constructible and move-assignable. It directly owns its native handle and has no shared implementation ownership.
- Callers keep at most one live environment for a database path in a process, as required by LMDB. The adapter deliberately has no canonical-path registry and does not attempt to detect aliases.
- At most one active write transaction in the process performs database-open calls at a time, including transactions for different environments.
- A write transaction acquires database-open admission lazily on its first main or named database open and releases it only after native commit or abort finishes.
- Lock acquisition is environment writer transaction before process-wide database-open admission. A thread that holds database-open admission does not begin or wait for a write transaction on another environment before releasing admission.
- A configured map size is a capacity bound rather than a disk reservation wherever the filesystem can hold a hole; where it cannot, the environment reports that its map costs the whole size instead of pretending otherwise.
- Reported high water is the peak page extent the environment has committed, never a live-data measure, and it does not decrease when records are deleted.
- Map capacity changes only at the open boundary, before the environment is handed to a caller. No live environment is resized, because resizing unmaps and remaps the file and would invalidate every pointer a transaction or reader holds.
- Capacity only grows. A policy whose floor or ceiling is below what a database already recorded leaves that database's map where it is.
- An exhausted map is its own recoverable code, distinct from a full disk and from an exhausted identifier space, because only the exhausted map may succeed on a repeat with more capacity.
- Environment paths crossing this adapter are UTF-8, matching LMDB's own decoding, and are converted explicitly rather than through a platform's narrow-string default.
- Native environment handles are private to the adapter; public callers compose transactions and databases rather than bypassing their ownership checks.
- A byte span, key view, iterator value, reader, or writer obtained from a transaction does not outlive the transaction that supplies its storage or cursor state.
- Explicitly aborting or destroying an uncommitted write transaction aborts all of its staged changes.
- An unexpected failure from a mutating LMDB primitive aborts the complete write transaction before control returns; swallowing the reported error cannot make earlier staged writes committable.
- Calling `commit()` consumes the native write transaction whether commit succeeds or fails; the transaction and every writer created from it are terminal afterward.
- A writer operation after its transaction's commit attempt violates `AO_EXPECTS` before touching the cursor.
- A read point miss, write-transaction point miss, delete miss, empty-database maximum, and iterator end are normal values rather than recoverable errors.
- A non-end cursor fault and a non-miss point-read fault are never collapsed into absence.
- Such a fault on an ordinary read snapshot is fatal; the same fault inside a write transaction uses the private transaction-failure carrier so the owner aborts the complete root.
- An exclusive create never overwrites an existing record.
- An update is an upsert: it replaces an existing value or creates the key when absent.
- Integer point operations, append, and maximum-key operations exist only on `IntegerKeyDatabase` readers and writers.
- Byte-span key operations exist only on `ByteKeyDatabase` readers and writers; a caller cannot select or query a runtime key kind.
- Public integer-key and byte-key create, update, and append operations accept copied input bytes only; mutable reserved storage is reachable only through the source-private Track reservation access.
- Only a successful write commit makes staged bytes visible to a later independent transaction.
- Error text is diagnostic; callers branch on `Error::Code` or the operation's value shape rather than parsing an LMDB message.

## State model

### Environment

An environment is either owned or moved-from.
Successful `Environment::open` produces the owned state with one native handle.
Move construction or assignment transfers that handle and leaves the source moved-from.
Destruction closes the native handle; the same process may then reopen that path.

### Transaction

| State | Read behavior | Write behavior | Transition |
|---|---|---|---|
| Active read | Snapshot reads and iteration are available. | Not available. | Destruction aborts/releases the native read transaction. |
| Active write | Snapshot reads and staged writes are available. | Create, update, append, delete, clear, commit, and abort are available. | `commit()`, `abort()`, a `detail::TransactionFailure`, or destruction makes the transaction terminal. |
| Terminal write | No transaction handle remains. | Writer use is invalid. | No transition back to active. |

`ReadTransaction::isActive()` reports whether a native handle remains.
`WriteTransaction::isFinished()` is its inverse and becomes true after successful commit, failed commit, explicit abort, or move-out, so it does not independently prove durable success; callers inspect the `Result` returned by `commit()`.

### Iterator

An iterator is positioned on one record or is end.
Construction seeks the first record; increment seeks the next record; `MDB_NOTFOUND` transitions to end.
Dereference and increment require the positioned state.

### Writer allocation cursor

An integer-key writer captures the database's largest key when the writer is constructed and advances that cached value after each successful append.
`Writer::maxKey()` exposes this cached allocation value and is distinct from `Reader::maxKey()`.
Successful `clear()` resets that cached allocation value to zero, so the same writer's next append allocates key `1`.

Explicit `create`, `update`, or `delete` calls do not recompute or advance the cache.
Callers that mix explicit integer-key creation with append in one writer must not assume append observes the newly explicit maximum; constructing a new writer after commit obtains the current database maximum.

## Commands and transitions

### Environment and database opening

`Environment::open` applies a nonzero pinned map size and the maximum-database and maximum-reader options before opening the requested path, then prepares that path's data file, then uses the supplied flags and mode for the native environment open, and finally applies the capacity policy while nothing has yet been handed out.
Failure at environment creation, option application, data-file preparation, path opening, or capacity application returns a recoverable `Result` error and closes any partially created native handle.

Three separate figures describe an environment's storage and this specification keeps them apart.
**Map capacity** is how much the environment may grow into before a mutation runs out of room, and it also bounds the address space the mapping reserves.
**File length** is what the data file reports as its size.
**Allocated bytes** is what the filesystem has actually given it.

The platforms relate those figures differently.
POSIX with `MDB_WRITEMAP` disabled, which is what Aobus uses, never extends the data file for the mapping: LMDB appends pages as it writes them, so length and allocation both follow committed use and the map size stays a capacity and address-space bound only.
Windows extends the file to the map size before creating the mapping, so its length always reports the whole map and, without a hole, allocates the whole map too.

Data-file preparation exists for that Windows case.
It opens or creates the data file and marks it sparse before the mapping fixes the length, so allocation follows committed use while the length still reports the map.
On POSIX it does nothing, because the mapping already behaves that way.
The step is idempotent, so concurrent first openers and every later open of the same environment repeat it safely.

Preparation reports which of the two allocation behaviours the environment got, and `Environment::mapAllocation()` exposes that decision for the environment's lifetime.
A volume that supports no sparse file at all reports `WholeMap` rather than failing, because the map size becoming allocation is a cost to price rather than a broken environment; choosing a map size such a volume can honour belongs to the caller that owns capacity rather than to this adapter.
Any other preparation failure is a recoverable `Result` error, so a genuine permission or device fault is not mistaken for a filesystem limitation.

Marking a file sparse does not release clusters it already allocated, so a data file an earlier dense build created keeps its allocation.
Reclaiming that unused tail requires the environment closed and its committed high water known, so it belongs to a caller that reopens the environment rather than to preparation.

The path this adapter receives is UTF-8, matching what LMDB itself decodes, and preparation converts it explicitly rather than relying on a platform's narrow-string default.

Opening admits only the two mirrored environment flags, `kEnvNoTls` and `kEnvReadOnly`, and rejects anything else as `InvalidInput`.
Data-file preparation assumes the environment directory holds the data file and that the mapping never extends it; `MDB_NOSUBDIR` makes the path the data file rather than its directory, and `MDB_WRITEMAP` turns an exhausted volume into a mapping fault instead of a returned error.
Both would break preparation silently, so an unrecognized flag is refused rather than trusted.

The open function takes a native path rather than an encoded string, because its two consumers need different encodings: platform preparation works on the native form, and LMDB decodes what it receives as UTF-8.
A caller that converted first would have to choose one and be wrong about the other where the platform's narrow encoding is not UTF-8.

A caller chooses between two ways of sizing an environment.
`Options::pinnedMapBytes` sets the capacity before the open, which overrides whatever the database recorded and admits no policy growth; it exists for callers that need one known capacity, including tests that have to reach the end of a map.
It is not a guaranteed exact size: LMDB silently raises a request below the space the environment has already consumed, so the effective map is never under the committed extent.
Leaving it zero lets LMDB adopt the size the database itself recorded, which is what allows a database to keep capacity an earlier session gave it, and a fresh database to start at LMDB's own small default.
`Options::capacity` then decides whether to raise that.

The capacity policy names two floors, a ceiling, and a growth step for a file that cannot hold a hole.
A default policy names neither floor nor ceiling and therefore changes nothing, so an environment sized by `pinnedMapBytes` alone behaves as it always did.
A ceiling of zero disables the growth rule alone: no step is taken and no headroom is reserved, while a floor above the current map still applies, because a floor is the capacity the caller asked to open with rather than a reaction to how full the database has become.
The two floors are separate because a floor above the recorded high water costs no disk where the map's unused remainder is a hole and is immediate disk usage where it is not, so one figure would have to be wrong for one of the two cases; `minimumMapBytes` applies to the first and `denseMinimumMapBytes` to the second, and a policy that names no floor for the case it got keeps whatever the database recorded.
Otherwise the planned capacity starts at the larger of the current map and the floor for that case, and then grows while the map leaves the recorded high water less than one further step of room, stopping at the ceiling.
A file that can hold a hole doubles, because its unused remainder costs nothing; a file that cannot grows by the configured step instead, because there every added byte is an allocated byte, and a policy that configures no step for that case leaves such a map at its floor.
The planned capacity is never below the current map, so a database opened once under a larger ceiling keeps its capacity afterwards.
Applying it uses the native map-size call on the just-opened environment, which is safe only here: that call unmaps and remaps the file, and LMDB documents that it does not check for active transactions, so the caller must guarantee there are none.
Construction order is that guarantee rather than any check, because the handle has not yet left the open function and no transaction on the environment can exist.
The new size takes effect for this process immediately but reaches the database only when a later write transaction commits something, so an open alone raises the map without recording it and the next open recomputes from what was recorded.
A read-only environment is left alone, since it maps whatever exists and could not record a larger size.
Data-file preparation is likewise skipped for one: creating an absent file, requesting write permission, and marking a file sparse are all mutations, so a read-only open reports the allocation the existing file already describes and touches the data file not at all.
That is a statement about the data file rather than about the whole environment, since LMDB still maintains the reader table in the lock file except on a read-only filesystem.

Growth reacts to the recorded high water, which a rolled-back mutation does not change.
A mutation that exhausts the map therefore leaves nothing behind for the next open to react to, and a caller that means to repeat that work has to name the capacity it wants through the policy floor rather than expect the database to ask for it.

`Environment::capacity()` reports that capacity alongside how much of it the environment has needed.
`mapBytes` is the configured or inherited map size, `pageBytes` is the database page size, and `highWaterBytes` covers every page up to the highest one the environment has ever committed.
The high water is a peak rather than a measure of live data: deleting records returns their pages to the free list for reuse, and nothing lowers the figure in place, so a capacity decision reads it as the amount the map has had to cover.
The accessor requires only an owned environment and reports no recoverable failure, because the native queries reject nothing else.
The adapter does not canonicalize or register paths; application composition owns LMDB's one-live-environment-per-path process constraint.
The independent write-transaction database-open admission serializes the native DBI-open interval across environment paths without entering later reads or writes through retained typed database tokens.

`IntegerKeyDatabase::open` and `ByteKeyDatabase::open` create the named database when missing and otherwise open the existing database with the type's exact native key flags.
Their `openExisting` factories never create and report a missing or incompatible catalog entry as a typed schema-admission result.
Named databases can be opened only through an active write transaction; a retained typed token can then create readers and writers for later transactions without reopening its DBI.
`ByteKeyDatabase::main` opens the main database only when its persistent key flags describe byte keys; a mismatch returns `CorruptData` instead of producing a falsely typed token.
An unexpected write-open failure throws `lmdb::detail::TransactionFailure`; the transaction owner must unwind, which aborts that complete transaction, including named databases created earlier in it.
Both create-capable and existing-only opens inspect the returned database's native flags instead of assuming that requested flags changed an existing database.
A mismatch between the factory's required and actual persistent flags is `CorruptData`, and no typed token with a false key interpretation is returned.

An `IntegerKeyDatabase` uses native `std::uint32_t` keys and integer ordering.
A `ByteKeyDatabase` accepts arbitrary byte-span keys and iterates in LMDB byte order.
Converting an integer iterator `KeyView` to `std::uint32_t` requires exactly four key bytes and fails through `AO_INVARIANT` on a size mismatch; byte-key iteration provides no integer conversion.

### Transaction begin and commit

Beginning a read or write transaction returns `Result<Transaction>`.
A successful write commit publishes all staged changes atomically.
Destruction without commit aborts the complete transaction.
Explicit `abort()` consumes the native handle immediately and is idempotent.
Commit, explicit abort, destruction, and move replacement finish the native transaction before releasing database-open admission.
Opening a database from an inactive transaction or creating a reader/writer from an inactive transaction violates `AO_EXPECTS` before calling LMDB.

### Reads and iteration

| Operation | Existing data | Missing or empty | Other LMDB fault |
|---|---|---|---|
| `Reader::get` | Transaction-scoped byte span. | `std::nullopt`. | Fatal on an ordinary read transaction; private transaction failure on a write transaction. |
| `Writer::get` | Transaction-scoped byte span. | `std::nullopt`. | Private transaction failure. |
| `Reader::maxKey` | Largest integer key. | `0`. | Same channel as `Reader::get`. |
| Iterator construction/increment | Current key/value view. | End sentinel. | Same channel as the owning transaction. |

Reader iteration follows database key order and yields borrowed key/value spans.
Integer `maxKey` reads the last database key at the time of the transaction snapshot; it is distinct from the writer's cached append cursor.

### Writes

| Operation | Behavior | Normal non-success | Failure channel |
|---|---|---|---|
| `create(key, data)` | Insert without overwrite. | Existing key is `Conflict`. | Other LMDB mutation faults throw `detail::TransactionFailure`. |
| `update(key, data)` | Upsert the supplied value. | None. | An LMDB mutation fault throws `detail::TransactionFailure`. |
| `IntegerKeyDatabase::Writer::append(data)` | Allocate cached maximum plus one and exclusively create it. | Exhaustion is `ResourceExhausted`. | Other create failure is propagated. |
| `del(key)` | Remove an existing record. | Missing key returns `false`; deletion returns `true`. | Other faults use `detail::TransactionFailure`. |
| `clear()` | Remove all records while retaining the named database; an integer writer also resets its cached append maximum to zero. | Empty clear succeeds. | An LMDB mutation fault throws `detail::TransactionFailure`. |

The public create, update, and append operations on `IntegerKeyDatabase::Writer` and `ByteKeyDatabase::Writer` accept only copied input bytes.
The source-private `detail::ReservationWriterAccess` is the sole mutable-reservation boundary, and its sole production consumer is library Track encoding.
It accepts a callback that is nothrow-invocable as `void(std::span<std::byte>)`, invokes it exactly once after a successful native reservation, and does not invoke it when reservation fails.
The mutable span is valid only for that synchronous invocation; Track preparation finishes before the call, and canonical post-fill validation completes before the callback returns.

`Conflict` from exclusive create, a delete miss, and append ID exhaustion are predefined no-mutation outcomes and leave the transaction active.
They are `Result` values because no logical mutation has been accepted.

That guarantee is relative to the individual adapter call, not to a higher-level item composed from several calls.
A caller that intends to catch a recoverable item rejection and continue in the same transaction must complete every fallible size, canonicality, and reconstruction check before the item's first mutating call.
After any call has staged part of that item, a later failure must reach the root operation boundary; returning it into code that can keep writing or commit would make a successful prefix observable.
The music-library layer uses preflight for safe item continuation and a terminalizing root boundary for post-effect failure; the adapter exposes no item-savepoint transaction API.

An LMDB failure from a mutating call after the operation has entered the mutation path throws `lmdb::detail::TransactionFailure` carrying the mapped `Error`.
This is private transaction-control flow, not a second public error vocabulary: the nearest library transaction owner catches it exactly, explicitly aborts all staged changes, and only then converts the carried error to the enclosing operation's `Result`.
The marker does not cross into runtime, CLI, or frontend code, and the failed root cannot be continued or committed while its wrapper remains alive.
A caller must not catch it inside the owner scope, continue using the writer, or attempt a later commit.
This defines item neutrality relative to the transaction state when that item began: unrelated mutations already staged by the owner are not part of the item delta, while the library's in-memory candidate revision is persisted only at the later commit boundary.

Append starts at key `1` for an empty integer database because key `0` is reserved by current Aobus consumers as a null identity.
When the cached maximum is `std::numeric_limits<std::uint32_t>::max()`, both the public copied-data append and the private reservation append return `ResourceExhausted` without attempting a write.
If exclusive creation fails after the cache was incremented, append restores the previous cached maximum and propagates the error.

## Failure and cancellation

The shared result adapter maps native codes as follows on `Result`-returning paths:

| Native result | `Error::Code` |
|---|---|
| `MDB_SUCCESS` | Successful value. |
| `MDB_NOTFOUND` | `NotFound`. |
| `MDB_KEYEXIST` | `Conflict`. |
| `MDB_MAP_FULL` | `StorageFull`. |
| `MDB_MAP_RESIZED` | `InvalidState`. |
| Any other LMDB code | `IoError`. |

The adapter prefixes the native LMDB diagnostic with the originating operation and captures the adapter call site that invokes the result-mapping helper.
Integer append exhaustion originates `ResourceExhausted` directly rather than mapping a native LMDB result.

The two capacity codes are separated from `IoError` because their remedies differ from every other storage failure and from each other.
`StorageFull` means the map ran out of room, which reaching this environment's page limit is the only way to produce; the same work may succeed in a process that opens the database with more capacity.
A full volume is not this code: LMDB reports the underlying write failure, which maps to `IoError`, and repeating that work with a larger map would fail again.
An exhausted identifier space is not this code either; it keeps `ResourceExhausted`, which no amount of capacity changes.
`MDB_TXN_FULL` describes a transaction holding too many dirty pages rather than a map that is full, and keeps `IoError`, because a larger map does not admit it.

`InvalidState` from `MDB_MAP_RESIZED` means another process committed past this process's map, so this environment's mapping is stale.
It originates only at transaction begin or renewal, which is where LMDB compares its map against the committed page count, so this adapter always returns it as a recoverable `Result` rather than through a fatal read path.
That is a statement about this adapter and not about every consumer: `MusicLibrary` treats a transaction that fails to begin as fatal, so the code is recoverable where it is produced and terminal where the library layer receives it.
LMDB itself allows a live environment to adopt the new size by calling the native map-size function with zero while no transaction is active in this process.
Aobus does not: no owner can quiesce every reader and cursor across the runtime to make that call safe, so at the product level the only recovery is reopening the environment, which adopts the larger recorded size.

This result mapping applies to operations that return `Result`, including environment opening, transaction begin and commit, and the documented no-mutation outcomes of create, append, and delete.
The database writer's mutation-fault path uses the same code mapping inside `detail::TransactionFailure` instead of returning a `Result`, because the transaction cannot safely continue.
Point reads and cursor construction or advance deliberately use value-or-fail contracts: only their documented miss/end state is normal.
An ordinary read transaction sends every other native failure to `AO_FATAL`; a write transaction throws `detail::TransactionFailure`, including failures from reads needed to decide a mutation.
The write owner catches that carrier exactly, aborts, and only then exposes its mapped `Error`.
Key-shape violations discovered after a typed database token has been admitted are invariants rather than recoverable native errors.

The adapter cannot provide recoverable containment for every storage failure.
LMDB uses a memory-mapped database; external truncation or corruption may surface as `SIGBUS`, which no `Result` boundary can intercept, and the same corruption may also appear as a thrown cursor fault.
Accordingly, a point lookup or cursor must never convert an arbitrary nonzero LMDB result into an empty value.

The music-library admission pass runs inside one uncommitted write transaction.
Consequently, native read faults during admission use the write transaction's private carrier and are translated by `MusicLibrary::open()` into its recoverable typed result, while the same fault after a successful open is fatal.
This is an ownership distinction, not a second Store read API.

Tests prove all three phases through the source-private `detail::ReadFaultInjection` scope rather than corrupting a mapped environment.
The scope arms one non-success code for the next LMDB read adapter call on the same thread and records its consumption.
Leaving the owning thread violates a lifecycle invariant; leaving the scope with an unconsumed injection enters `AO_FATAL` after clearing the test slot because that branch is already known to be unconditionally terminal.
A build guard prevents production roots from including or naming the seam; the normal read path performs only the thread-local empty check and no write when no injection is armed.

All operations are synchronous and expose no cooperative cancellation point.
Application cancellation and task scheduling occur above this core adapter and cannot reinterpret a completed commit.

## Persistence and versioning

The adapter supplies LMDB transactional durability but assigns no Aobus schema, record version, migration, or application revision semantics.
Only successful write commit is a durability boundary.

The exact host-local music-library environment, named databases, keys, records, and version gate belong to the [library database reference](../../reference/library/storage/database.md).
Portable interchange belongs to the library YAML format and is not an LMDB adapter concern.

## Frontend observations

The LMDB adapter has no direct frontend observation or reporting surface.
Core library stores interpret raw bytes and runtime services translate their results into application operations, revisioned events, or reporting decisions.

UIModel and normal frontends consume retained values and snapshots rather than transaction-bound spans.

## Implementation map

- [`Environment.h`](../../../include/ao/lmdb/Environment.h) and [`Environment.cpp`](../../../lib/lmdb/Environment.cpp) own environment options, opening, handle lifetime, and capacity reporting.
- [`EnvironmentDataFile.h`](../../../lib/lmdb/detail/EnvironmentDataFile.h), with
  [`EnvironmentDataFileWindows.cpp`](../../../lib/lmdb/detail/EnvironmentDataFileWindows.cpp) and
  [`EnvironmentDataFilePosix.cpp`](../../../lib/lmdb/detail/EnvironmentDataFilePosix.cpp), owns the pre-open
  data-file preparation that keeps a map size from becoming disk usage.
- [`MapCapacityPolicy.h`](../../../lib/lmdb/detail/MapCapacityPolicy.h) and
  [`MapCapacityPolicy.cpp`](../../../lib/lmdb/detail/MapCapacityPolicy.cpp) own the pure grow-only rule that
  turns a reported capacity, its allocation behaviour, and a policy into the map size the open boundary installs.
- [`Transaction.h`](../../../include/ao/lmdb/Transaction.h) and [`Transaction.cpp`](../../../lib/lmdb/Transaction.cpp) own read/write begin, process-wide database-open admission, abort, commit, and terminal state.
- [`Database.h`](../../../include/ao/lmdb/Database.h) and [`Database.cpp`](../../../lib/lmdb/Database.cpp) own the integer-key and byte-key tokens, named-database access, readers, iterators, writers, and integer key allocation.
- [`ReservationWriterAccess.h`](../../../lib/lmdb/detail/ReservationWriterAccess.h) owns the source-private zero-copy reservation access used by library Track encoding; public create, update, and append operations accept only copied input bytes.
- [`UnvalidatedDatabase.h`](../../../lib/lmdb/detail/UnvalidatedDatabase.h) owns the source-private, read-only token used to inspect a library metadata version before consuming the same DBI as an integer-key token; the LMDB build guard limits that seam to its implementation and `MusicLibrary` admission.
- [`ResultError.h`](../../../lib/lmdb/detail/ResultError.h) owns recoverable native-code mapping and source-location capture.
- [`ThrowError.h`](../../../lib/lmdb/detail/ThrowError.h) dispatches native read faults to the owning transaction channel and constructs mutation-failure control flow; the private [`TransactionFailure.h`](../../../lib/lmdb/detail/TransactionFailure.h) carries the narrow transaction-control error to an owner boundary.
- [`ReadFaultInjection.h`](../../../lib/lmdb/detail/ReadFaultInjection.h) and
  [`ThrowError.cpp`](../../../lib/lmdb/detail/ThrowError.cpp) own the
  source-private, single-use native-read test seam; [`lib/lmdb/CMakeLists.txt`](../../../lib/lmdb/CMakeLists.txt)
  guards its production boundary.
- [`DatabaseOpenAdmissionProbe.h`](../../../lib/lmdb/detail/DatabaseOpenAdmissionProbe.h) and
  [`Transaction.cpp`](../../../lib/lmdb/Transaction.cpp) own the source-private
  contention observation used by deterministic gate tests; the LMDB build
  guard prevents production consumers from constructing it.
- [`lib/lmdb/CMakeLists.txt`](../../../lib/lmdb/CMakeLists.txt) defines the independent `ao_lmdb` target and external dependency.

## Test map

- [`EnvironmentTest.cpp`](../../../test/unit/lmdb/EnvironmentTest.cpp) protects environment opening, errors, private native-handle access, move-only ownership, capacity reporting including the high water's refusal to fall after a clear, and the open boundary's capacity decisions: opening a fresh database at the policy floor, keeping a larger recorded map across a reopen, raising the map once the peak passes half of it, stopping at the ceiling, and leaving a pinned map size alone.
- [`EnvironmentDataFileTest.cpp`](../../../test/unit/lmdb/EnvironmentDataFileTest.cpp) protects allocation staying proportional to committed use under a large configured map, across a reopen, over a data file an earlier session left behind, on a non-ASCII path, and under deterministic concurrent preparation and concurrent first opens.
- [`TransactionTest.cpp`](../../../test/unit/lmdb/TransactionTest.cpp) protects top-level-only construction, read/write lifetime, commit, abort, moves, and bounded deterministic database-open serialization plus commit/abort/destruction release across environments.
- [`DatabaseTest.cpp`](../../../test/unit/lmdb/DatabaseTest.cpp) protects write-transaction-only database admission, create-capable and existing-only exact flag validation for both named-token types, exact byte-key main-database validation, missing existing databases, and failed-open rollback.
- [`DatabaseReaderTest.cpp`](../../../test/unit/lmdb/DatabaseReaderTest.cpp), [`DatabaseByteKeyTest.cpp`](../../../test/unit/lmdb/DatabaseByteKeyTest.cpp), and [`DatabaseMaxKeyTest.cpp`](../../../test/unit/lmdb/DatabaseMaxKeyTest.cpp) protect miss/end values, byte-key ordering and operations, compile-time key-operation isolation, integer-key coercion, and maximum-key behavior.
- [`DatabaseWriterTest.cpp`](../../../test/unit/lmdb/DatabaseWriterTest.cpp) protects compile-time key-operation isolation, the copied-data-only public writer surface, source-private integer reservation encoding and append, clear-and-reappend allocation, exhaustion, update, delete, write reads, conflicts, mutation-failure exception unwinding and rollback, moves, and use-after-commit faults.
- [`MapCapacityPolicyTest.cpp`](../../../test/unit/lmdb/MapCapacityPolicyTest.cpp) protects the grow-only rule: a default policy leaving the map alone, the floor lifting a smaller map without lowering a larger one, doubling once the peak passes half and repeating until it fits, the ceiling stopping growth without shrinking a map already past it, additive growth and its absence where a file holds no hole, and saturation near the representable limit.
- [`ResultErrorTest.cpp`](../../../test/unit/lmdb/ResultErrorTest.cpp) protects native-code mapping including the exhausted-map, stale-mapping, full-disk, and full-transaction separation, and caller source-location capture.
- [`MusicLibraryTest.cpp`](../../../test/unit/library/MusicLibraryTest.cpp),
  [`WriteTransactionTest.cpp`](../../../test/unit/library/WriteTransactionTest.cpp),
  and the library fatal subprocess scenarios under
  [`test/fatal/`](../../../test/fatal) use the governed read-fault seam to
  protect admission, writer-root, and post-open ownership respectively.

## Related documents

- [System architecture](../../architecture/system-overview.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Library architecture](../../architecture/library.md)
- [Outcome channel specification](../failure/outcome-channel.md)
- [Library database reference](../../reference/library/storage/database.md)
