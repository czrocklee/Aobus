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
The private mutation-failure carrier is implemented under `lib/lmdb/detail/` and is available to `ao_library` only through a private build dependency.
The `ao_library` core target may otherwise depend on the public `ao_lmdb` surface; the adapter does not depend on library stores, application runtime, UIModel, or frontends.

Runtime, UIModel, and normal frontend public boundaries do not expose LMDB environments, transactions, cursors, database handles, or transaction-scoped byte spans.

## Terminology

- An **environment** owns one native LMDB environment handle.
- A **read transaction** owns one read snapshot until destruction.
- A **write transaction** owns staged mutations until commit or destruction and also provides read capability.
- A **nested write transaction** stages changes into its parent; only the outer transaction can make them durable.
- A **database** is one named LMDB database handle plus its integer-key or blob-key interpretation.
- A **reader** borrows a transaction and returns byte views into that transaction's snapshot.
- A **writer** borrows an active write transaction and owns a cursor for its database.
- **End** is the normal cursor state represented by `MDB_NOTFOUND` during iteration.
- An **operational fault** is an LMDB failure other than the normal absence/end cases declared by a particular operation.

## Invariants

- Environments, transactions, iterators, writers, and their native handles follow RAII ownership and are movable but not copyable unless their public type explicitly provides copying.
- A byte span, key view, iterator value, reader, or writer obtained from a transaction does not outlive the transaction that supplies its storage or cursor state.
- Explicitly aborting or destroying an uncommitted write transaction aborts all of its staged changes.
- An unexpected failure from a mutating LMDB primitive aborts the complete write transaction before control returns; swallowing the reported error cannot make earlier staged writes committable.
- Calling `commit()` consumes the native write transaction whether commit succeeds or fails; the transaction and every writer created from it are terminal afterward.
- A writer operation after its transaction's commit attempt throws before touching the cursor.
- A read point miss, write-transaction point miss, delete miss, empty-database maximum, and iterator end are normal values rather than recoverable errors.
- A non-end cursor fault and a non-miss point-read fault throw; they are never collapsed into absence.
- An exclusive create never overwrites an existing record.
- An update is an upsert: it replaces an existing value or creates the key when absent.
- Integer overloads, append, and maximum-key operations are used with `KeyKind::Integer`; byte-span key operations are used with `KeyKind::Blob`.
- The adapter does not dynamically reject every key-kind/overload mismatch, so callers preserve this pairing as a public precondition.
- Only a successful outermost write commit makes staged bytes visible to a later independent transaction.
- Error text is diagnostic; callers branch on `Error::Code` or the operation's value shape rather than parsing an LMDB message.

## State model

### Environment

An environment is either owned or moved-from.
Successful `Environment::open` produces the owned state; destruction closes the native handle.

### Transaction

| State | Read behavior | Write behavior | Transition |
|---|---|---|---|
| Active read | Snapshot reads and iteration are available. | Not available. | Destruction aborts/releases the native read transaction. |
| Active write | Snapshot reads and staged writes are available. | Create, update, append, delete, clear, child begin, commit, and abort are available. | `commit()`, `abort()`, a `detail::TransactionFailure`, or destruction makes the transaction terminal. |
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

Explicit `create`, `update`, or `delete` calls do not recompute or advance the cache.
Callers that mix explicit integer-key creation with append in one writer must not assume append observes the newly explicit maximum; constructing a new writer after commit obtains the current database maximum.

## Commands and transitions

### Environment and database opening

`Environment::open` applies nonzero map-size, maximum-database, and maximum-reader options before opening the requested path, then uses the supplied flags and mode for the native environment open.
Failure at environment creation, option application, or path opening returns a recoverable `Result` error and releases any partially created native handle.

Opening a database through a write transaction creates the named database when missing and otherwise opens the existing database.
An unexpected write-open failure throws `lmdb::detail::TransactionFailure`; the transaction owner must unwind, which aborts that complete transaction, including named databases created earlier in it.
Opening through a read transaction never creates; a missing database returns `NotFound`.

An integer-key database uses native `std::uint32_t` keys and integer ordering.
A blob-key database accepts arbitrary byte-span keys and iterates in LMDB byte order.
Converting an iterator `KeyView` to `std::uint32_t` requires exactly four key bytes and throws on a size mismatch.

### Transaction begin, nesting, and commit

Beginning a read or write transaction returns `Result<Transaction>`.
Beginning a child write transaction borrows an active parent write transaction.

A successful child commit merges the child's staged state into its parent but does not make it independently durable.
Destroying an uncommitted child aborts only the child changes.
The parent remains responsible for the final commit or abort.

A successful outer commit publishes all staged changes atomically.
Destruction without commit aborts the complete transaction.
Explicit `abort()` consumes the native handle immediately and is idempotent.
Beginning a child, opening a database, or creating a reader/writer from an inactive transaction fails before calling LMDB.

### Reads and iteration

| Operation | Existing data | Missing or empty | Other LMDB fault |
|---|---|---|---|
| `Reader::get` / `Writer::get` | Transaction-scoped byte span. | `std::nullopt`. | Throw `ao::Exception`. |
| `Reader::maxKey` | Largest integer key. | `0`. | Throw `ao::Exception`. |
| Iterator construction/increment | Current key/value view. | End sentinel. | Throw `ao::Exception`. |

Reader iteration follows database key order and yields borrowed key/value spans.
Integer `maxKey` reads the last database key at the time of the transaction snapshot; it is distinct from the writer's cached append cursor.

### Writes

| Operation | Behavior | Normal non-success | Failure channel |
|---|---|---|---|
| `create(key, data)` | Insert without overwrite. | None. | Existing key is `Conflict`; an LMDB mutation fault throws `detail::TransactionFailure`. |
| `create(key, size)` | Reserve a new value of the requested size for caller fill. | None. | Same as exclusive create. |
| `update(key, data)` | Upsert the supplied value. | None. | An LMDB mutation fault throws `detail::TransactionFailure`. |
| `update(key, size)` | Upsert and reserve the requested value size. | None. | An LMDB mutation fault throws `detail::TransactionFailure`. |
| `append(data)` | Allocate cached maximum plus one and exclusively create it. | None. | Exhaustion is `ResourceExhausted`; create failure is propagated. |
| `append(size)` | Allocate and reserve cached maximum plus one. | None. | Same as data append. |
| `del(key)` | Remove an existing record. | Missing key returns `false`; deletion returns `true`. | Other faults throw rather than returning `Result`. |
| `clear()` | Remove all records while retaining the named database. | Empty clear succeeds. | An LMDB mutation fault throws `detail::TransactionFailure`. |

Reservation spans are writable transaction-scoped storage.
The caller must overwrite every reserved byte before the next LMDB update operation in that transaction or before the transaction finishes.
The caller must not read or write the reservation after either boundary.
Code that needs more than one reservation therefore fills and consumes them sequentially rather than retaining several writable spans.

`Conflict` from exclusive create, a delete miss, and append ID exhaustion are predefined no-mutation outcomes and leave the transaction active.
They are `Result` values because no logical mutation has been accepted.

That guarantee is relative to the individual adapter call, not to a higher-level item composed from several calls.
A caller that intends to catch a recoverable item rejection and continue in the same transaction must complete every fallible size, canonicality, and reconstruction check before the item's first mutating call.
After any call has staged part of that item, a later failure must reach the root operation boundary; returning it into code that can keep writing or commit would make a successful prefix observable.
The music-library layer uses preflight for safe item continuation and a terminalizing root boundary for post-effect failure; it does not use nested write transactions as item savepoints.

An LMDB failure from a mutating call after the operation has entered the mutation path throws `lmdb::detail::TransactionFailure` carrying the mapped `Error`.
This is private transaction-control flow, not a second public error vocabulary: the nearest library transaction owner catches it exactly, explicitly aborts all staged changes, and only then converts the carried error to the enclosing operation's `Result`.
The marker does not cross into runtime, CLI, or frontend code, and the failed root cannot be continued or committed while its wrapper remains alive.
A caller must not catch it inside the owner scope, continue using the writer, or attempt a later commit.
This defines item neutrality relative to the transaction state when that item began: unrelated mutations already staged by the owner, including the library revision bump created with a library write transaction, are not part of the item delta.

Append starts at key `1` for an empty integer database because key `0` is reserved by current Aobus consumers as a null identity.
When the cached maximum is `std::numeric_limits<std::uint32_t>::max()`, both append variants return `ResourceExhausted` without attempting a write.
If exclusive creation fails after the cache was incremented, append restores the previous cached maximum and propagates the error.

## Failure and cancellation

The shared result adapter maps native codes as follows on `Result`-returning paths:

| Native result | `Error::Code` |
|---|---|
| `MDB_SUCCESS` | Successful value. |
| `MDB_NOTFOUND` | `NotFound`. |
| `MDB_KEYEXIST` | `Conflict`. |
| Any other LMDB code | `IoError`. |

The adapter prefixes the native LMDB diagnostic with the originating operation and captures the adapter call site that invokes the result-mapping helper.
Integer append exhaustion originates `ResourceExhausted` directly rather than mapping a native LMDB result.

This result mapping applies to operations that return `Result`, including environment opening, transaction begin and commit, and the documented no-mutation outcomes of create, append, and delete.
The database writer's mutation-fault path uses the same code mapping inside `detail::TransactionFailure` instead of returning a `Result`, because the transaction cannot safely continue.
Point reads, cursor construction/advance, key coercion, and delete deliberately use value-or-throw contracts: only their documented miss/end state is normal, and every other failure throws `ao::Exception` (or `detail::TransactionFailure` for a delete mutation fault).

The adapter cannot provide recoverable containment for every storage failure.
LMDB uses a memory-mapped database; external truncation or corruption may surface as `SIGBUS`, which no `Result` boundary can intercept, and the same corruption may also appear as a thrown cursor fault.
Accordingly, a point lookup or cursor must never convert an arbitrary nonzero LMDB result into an empty value.

All operations are synchronous and expose no cooperative cancellation point.
Application cancellation and task scheduling occur above this core adapter and cannot reinterpret a completed commit.

## Persistence and versioning

The adapter supplies LMDB transactional durability but assigns no Aobus schema, record version, migration, or application revision semantics.
Nested child commit is not a durability boundary; only successful outer commit is.

The exact host-local music-library environment, named databases, keys, records, and version gate belong to the [library database reference](../../reference/library/storage/database.md).
Portable interchange belongs to the library YAML format and is not an LMDB adapter concern.

## Frontend observations

The LMDB adapter has no direct frontend observation or reporting surface.
Core library stores interpret raw bytes and runtime services translate their results into application operations, revisioned events, or reporting decisions.

UIModel and normal frontends consume retained values and snapshots rather than transaction-bound spans.

## Implementation map

- [`Environment.h`](../../../include/ao/lmdb/Environment.h) and [`Environment.cpp`](../../../lib/lmdb/Environment.cpp) own environment options, opening, and handle lifetime.
- [`Transaction.h`](../../../include/ao/lmdb/Transaction.h) and [`Transaction.cpp`](../../../lib/lmdb/Transaction.cpp) own read/write begin, nesting, abort, commit, and terminal state.
- [`Database.h`](../../../include/ao/lmdb/Database.h) and [`Database.cpp`](../../../lib/lmdb/Database.cpp) own named-database access, readers, iterators, writers, and key allocation.
- [`ResultError.h`](../../../lib/lmdb/detail/ResultError.h) owns recoverable native-code mapping and source-location capture.
- [`ThrowError.h`](../../../lib/lmdb/detail/ThrowError.h) owns value-or-throw native fault conversion and mutation-failure exception construction; the private [`TransactionFailure.h`](../../../lib/lmdb/detail/TransactionFailure.h) carries the narrow transaction-control error to an owner boundary.
- [`lib/lmdb/CMakeLists.txt`](../../../lib/lmdb/CMakeLists.txt) defines the independent `ao_lmdb` target and external dependency.

## Test map

- [`EnvironmentTest.cpp`](../../../test/unit/lmdb/EnvironmentTest.cpp) protects environment opening, errors, defaults, and move ownership.
- [`TransactionTest.cpp`](../../../test/unit/lmdb/TransactionTest.cpp) protects read/write lifetime, commit, abort, moves, and nested transaction behavior.
- [`DatabaseTest.cpp`](../../../test/unit/lmdb/DatabaseTest.cpp) protects named-database creation, failed-open rollback, and read-only `NotFound`.
- [`DatabaseReaderTest.cpp`](../../../test/unit/lmdb/DatabaseReaderTest.cpp), [`DatabaseBlobKeyTest.cpp`](../../../test/unit/lmdb/DatabaseBlobKeyTest.cpp), and [`DatabaseMaxKeyTest.cpp`](../../../test/unit/lmdb/DatabaseMaxKeyTest.cpp) protect miss/end values, iteration, key kinds, key coercion, and maximum-key behavior.
- [`DatabaseWriterTest.cpp`](../../../test/unit/lmdb/DatabaseWriterTest.cpp) protects create, reservation, append, exhaustion, update, delete, write reads, conflicts, mutation-failure exception unwinding and rollback, moves, and use-after-commit faults.
- [`ResultErrorTest.cpp`](../../../test/unit/lmdb/ResultErrorTest.cpp) protects native-code mapping and caller source-location capture.

## Related documents

- [System architecture](../../architecture/system-overview.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Library architecture](../../architecture/library.md)
- [Outcome channel specification](../failure/outcome-channel.md)
- [Library database reference](../../reference/library/storage/database.md)
