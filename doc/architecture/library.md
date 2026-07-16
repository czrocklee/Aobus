---
id: architecture.library
type: architecture
status: current
domain: library
summary: Defines ownership and data flow across music-library storage, runtime access roles, change publication, sources, and projections.
---
# Library architecture

## Scope

This document owns the structural boundary from physical music-library storage to frontend-neutral runtime views.
It defines storage ownership, runtime access roles, mutation publication, source caching, projection ownership, and the allowed dependency direction between them.

It does not define LMDB keys or record layouts, import/export schemas, scan classifications, list behavior, query grammar, or projection delta semantics.
Those details belong to the [library specifications](../spec/library/README.md) and [library reference](../reference/library/README.md).

## System context

The library subsystem has four architectural stages:

```text
ao::library::MusicLibrary
  physical stores + transactions
            |
            v
ao::rt::Library
  reader | writer | task service | changes
            |
            v
TrackSourceCache + TrackSource implementations
            |
            v
LiveTrackListProjection / LiveTrackDetailProjection
            |
            v
ViewService, workspace, playback sequence, UIModel/frontends
```

`CoreRuntime` owns one instance of this graph for one music root and database path.
The graph is frontend-neutral and is shared by interactive applications and CLI library operations.

The stages correspond directly to the [system architecture](system-overview.md):

| Library stage | System layer | Public code boundary | Implementation |
|---|---|---|---|
| Physical storage | Core libraries | `include/ao/library/` | `lib/library/` |
| Runtime library facade | Application runtime | `app/include/ao/rt/library/` | `app/runtime/library/` |
| Track sources | Application runtime | `app/include/ao/rt/source/` | `app/runtime/source/` |
| Live projections | Application runtime | `app/include/ao/rt/projection/` | `app/runtime/projection/` |

UIModel and frontends begin above these stages and consume runtime values rather than joining the Library implementation boundary.

## Responsibilities

### Physical storage

`ao::library::MusicLibrary` owns the LMDB environment and coordinates specialized track, list, resource, dictionary, and file-manifest stores.
It creates public read transactions and owns the physical library metadata header and revision source.
Committing writes require a separately acquired `WritableMusicLibrary`; `MusicLibrary` keeps transaction construction private to that capability.
Acquisition takes a non-blocking OS file lease for the database path, so a second writable process receives `Conflict` while the first capability or any transaction anchored to it remains active.
The capability borrows its `MusicLibrary`; storage composition keeps that library alive until the capability and all transactions anchored to its lease are destroyed.
Read-only processes do not take that lease.
The core [LMDB operation specification](../spec/storage/lmdb-operation.md) owns environment, transaction, cursor, and raw read/write behavior below these library-specific stores.

Every `MusicLibrary` read uses one move-only `ReadTransaction` that directly owns a native LMDB read transaction.
The wrapper is the library-level snapshot capability: store readers accept it, while its native handle remains private to `MusicLibrary` and the stores.
The wrapper and every store carry the same stable implementation-owned library identity, so a snapshot from one `MusicLibrary` is rejected before it can be mixed with another library's DBI.
This adds no allocation, locking, or another transaction layer to each operation.

Every writable-capability write uses one move-only `WriteTransaction` that owns the native LMDB transaction, the process writer gate, a shared writer-lease anchor, and a transaction-local dictionary writer.
Interning first consults committed mappings and then the transaction overlay; new id/text rows are written into the same native transaction as the track or other record that references them.
Dropping or failing the wrapper aborts both authorities.
Commit or abort consumes the native handle but retains the native transaction object and dictionary writer until the outer wrapper is destroyed.
Store writers that remain in ordinary scope across `commit()` therefore observe a terminal transaction and can be destroyed safely; any post-terminal operation fails before touching an LMDB cursor.

Before native commit, the wrapper acquires the dictionary's exclusive lock and prepares every potentially throwing in-memory insertion.
It holds that lock through commit, then either advances the dictionary generation and unlocks or rolls the prepared delta back before unlocking.
Readers therefore observe either the complete old mapping or the complete new mapping, and application change publication happens only after the latter is visible.

Committed dictionary ids form a dense, append-only range beginning at one; aborted tail ids may be reused, while committed ids are never reclaimed or rebound.
`DictionaryStore` serializes committed index publication and lookup with its internal shared mutex.
Published strings are immutable and use stable storage, so a borrowed view remains valid until the store is destroyed even when later commits grow the dictionary.
`DictionaryReadCache` is a bounded, owner-thread batch accelerator over those views rather than a snapshot or a wider lock scope.
Its collision replacement may cause another store lookup but cannot change a result; empty values are simply not retained.
`DictionaryReadContext` is the bounded synchronous read/binding port used by query and format evaluation.

Store types own physical representation and transaction-scoped access.
`MusicLibrary` exposes stores as const service handles; read capability comes from `ReadTransaction` or `WriteTransaction`, and mutation additionally requires a mutable `WriteTransaction`.
Raw LMDB transactions are not part of the public store operation surface.
They do not publish application events or construct frontend projections.

`TrackStore` owns both point reads and ordered batch reads of track records.
Its batch boundary preserves the caller's requested ID order, skips missing
rows, and retains duplicate requests. It chooses between point lookup and
coordinated cursor traversal internally, so smart-list evaluators and
projections do not duplicate LMDB access policy. Combined hot/cold traversal is
an ID merge join: only IDs present in both stores produce a combined view.

### Runtime library facade

`ao::rt::Library` is the application access boundary over `MusicLibrary`.
It exposes four cooperating roles and owns one private mutation coordinator:

- `LibraryReader` owns one read transaction for a coherent point-in-time read batch.
- `LibraryWriter` owns synchronous semantic commands; every effective command commits and publishes through the coordinator.
- `LibraryTaskService` owns long-running asynchronous operations such as scan, import/export, and identity backfill.
- `LibraryChanges` is the read-only revisioned mutation and task-progress observation boundary.
- `LibraryMutationService` exclusively owns the writable core capability, interactive/maintenance admission, commit revision checks, and publication completion.

The facade borrows storage, async runtime, and change-bus collaborators owned by `CoreRuntime`.
It groups roles and lifetime; the coordinator is an application control plane over the existing LMDB transaction system rather than another database or nested transaction layer.

The coordinator publishes `LibraryAuthoringAvailability` as `Available`, `Maintenance`, or terminal `Faulted` state.
Maintenance identifies import, scan apply, or audio-identity backfill and rejects every interactive command for the whole operation, including slow preparation between bounded write transactions.
Metadata and tag authoring additionally requires runtime-created `BoundTrackTargets` containing the runtime instance id, committed library revision, and exact target order.

### Sources

`TrackSource` is the runtime boundary for an ordered, observable set of track identities.
`TrackSourceCache` owns the all-tracks source, cached list sources, smart-list evaluation, dependency links between lists, and reusable ad-hoc filtered sources.

One `SmartListEvaluator` bucket rebuild creates one batch-local dictionary read cache/context, binds each immutable plan once, and shares those bindings across the tracks evaluated in that batch.
Binding resolves all plan symbols under one shared dictionary lock; later id-to-text cache misses take bounded point-read locks rather than delaying dictionary writers for a whole scan.

Callers acquire leases rather than taking raw ownership of cached sources.
The cache observes `LibraryChanges` and turns committed storage changes into source refreshes or incremental source deltas.

### Projections and views

Live projections combine a source lease with library reads and presentation structure.
They own frontend-neutral row/detail snapshots and publish projection deltas to consumers such as `ViewService` and `PlaybackSequenceService`.

`LiveTrackListProjection` resolves each dictionary ID into one cached pair: raw presentation text borrowed from `DictionaryStore` and a normalized sort/group key owned by its `StringArena`.
The cache is projection-local and owner-thread confined; a full rebuild releases every dependent row and section view, clears the cache, and then reclaims the arena.

`ViewService` owns the lifecycle of open runtime views and their projections.
Its track-list state keeps the content axis (`listId` plus a transient `filterExpression`) separate from the shape axis (`TrackPresentationSpec`).
The [track expression architecture](track-expression.md) owns how expression text reaches a predicate-backed source, while the [presentation architecture](presentation.md) owns how presentation state reaches UIModel and frontends.
UIModel and frontends consume runtime snapshots and commands rather than opening storage transactions to reconstruct the same view independently.

## Boundaries and dependency direction

- Physical stores depend only on lower library/LMDB facilities and do not depend on runtime.
- Runtime library implementations may depend on `MusicLibrary`, but public application consumers use `ao::rt::Library` roles and value types.
- Sources consume committed library state and change events; storage does not know that sources exist.
- Smart and ad-hoc sources consume the core expression system, but the expression system does not own source identity, ordering, leases, or deltas.
- Projections consume source leases and library reads; sources do not depend on projections or frontends.
- A projection combines source membership with presentation structure without allowing either concern to redefine the other.
- View, workspace, completion, and playback services consume sources/projections through runtime-owned boundaries.
- UIModel and normal frontend adapters do not include LMDB or concrete library store/view headers and cannot name committing write authority.

`CoreRuntime::musicLibrary()` is const and supports read-only CLI inspection and narrow runtime evaluator composition.
It cannot create a library write transaction.
Build guardrails reject write-transaction, writable-capability, and direct `LibraryWriter` dependencies from UIModel, GTK, and TUI; normal frontend mutation must cross UIModel or another semantic runtime command.

## Data and control flow

A synchronous mutation follows this path:

```text
runtime command
  -> LibraryWriter
  -> LibraryMutationService admission
  -> WriteTransaction + transaction-local dictionary overlay
  -> one LMDB commit with records, dictionary rows, and new library revision
  -> complete dictionary-index publication
  -> ordered LibraryChanges publication on the callback executor
  -> TrackSourceCache refresh/delta
  -> live projections
  -> view/playback/UI observers
  -> Available(runtimeInstanceId, committedRevision)
```

An asynchronous mutating operation enters exclusive maintenance before it leaves the callback executor, performs slow preparation through `LibraryTaskService` on the async worker pool without writer ownership, and acquires a bounded coordinator mutation only for apply/commit.
Export and scan-plan construction remain independent read snapshots.
Progress and completion return through `LibraryChanges`, while committed content changes use `LibraryChangeSet`.

A scan plan is an opaque move-only runtime value whose immutable items are bound to the persisted library id and committed revision from the planner's read snapshot.
Scan apply validates that evidence after maintenance admission and again at its bounded write boundary, so callers cannot fabricate items, cross libraries, or replay an already superseded snapshot.
Explicit relink is a constrained plan derivation that preserves the same binding rather than a separate caller-authored mutation description.

A read-oriented workflow obtains one `LibraryReader`, performs the related reads under its single transaction snapshot, and releases the reader before retaining application values.

Metadata and tag authoring first binds the exact targets to one runtime instance and one available committed revision.
Commit rechecks runtime identity, availability, revision, and every target under coordinator writer ownership.
A foreign or superseded binding is `Stale`, a missing target rejects the whole command as `Missing`, maintenance is `Unavailable`, and an effective commit returns a binding advanced to the published revision.

A filtered runtime view follows a separate composition path:

```text
base ListId + filter expression
  -> TrackSourceCache
  -> base or ad-hoc TrackSource
  -> LiveTrackListProjection + TrackPresentationSpec
  -> ViewService observers
```

Changing the filter replaces the active source/projection resources while retaining presentation state.
Changing presentation reshapes the projection without changing base-list or filter identity.

## Structural constraints

- One `MusicLibrary` instance and its runtime facade belong to one `CoreRuntime` and one music root.
- A scan plan can mutate only the library id and immediate successor revision captured by its construction snapshot.
- A library transaction is accepted only by stores carrying the same stable `MusicLibrary` identity.
- Library write transactions are process-serialized and non-nested; dictionary mappings are append-only within one open library.
- One OS lease excludes another writable process, and an active transaction retains that lease even if its originating capability is destroyed.
- Live-runtime commits can begin only through the one coordinator-owned writable capability.
- A mutation becomes observable through the revisioned change bus only after its write transaction commits.
- The coordinator admits the next mutation only after publication completion, and callback-thread reentrant mutation during publication is rejected.
- Consumers use published track and list identities to refresh state; they do not retain transaction-bound core views beyond their scope.
- `LibraryChanges` accepts publication only from the coordinator and serializes contiguous revision delivery onto the callback executor even when worker producers finish out of order.
- Source caches and projections derive state from storage plus the ordered change stream; they are not independent persistence authorities.
- A source lease pins source lifetime for its consumer, while the cache may otherwise evict unused implementations.
- Dictionary read caches never extend a store view beyond the owning `MusicLibrary`, and they do not provide transaction isolation or a dictionary snapshot.
- Projection raw-text caches borrow stable dictionary storage, while normalized projection keys never outlive the projection arena that owns them.
- Exact persistence records and exact delta operations are delegated to reference and specification documents.

## Failure, cancellation, and lifetime boundaries

Synchronous readers and writers finish their transaction scope before returning application values or publishing events.
`LibraryTaskService` owns the worker/callback transition for long-running operations and accepts cooperative stop tokens.
Executor hops honor cancellation, while only operations with explicit synchronous checkpoints can stop during their core work; [library task execution](../spec/library/runtime/task-execution.md#cancellation) owns the operation matrix.
Cancellation never reinterprets an already committed transaction as uncommitted.

Failure before commit returns through the operation's typed error channel and leaves the prior availability intact.
Once durable commit succeeds, enqueue or observer failure is a committed-publication fault: the coordinator enters terminal `Faulted`, rejects further live-runtime writes, and requires runtime reopen to rebuild derived state.

`CoreRuntime` destroys source, completion, facade, change-bus, and storage collaborators only after worker tasks are stopped and joined.
Subscriptions held by sources and projections release before the `LibraryChanges` owner they observe.
Batch and projection dictionary caches are destroyed before the `MusicLibrary` that owns their borrowed raw views.

Recoverable storage and external-data failures cross the runtime facade as typed results.
Shared channel behavior belongs to the [outcome channel specification](../spec/failure/outcome-channel.md), and exact common codes belong to the [error value reference](../reference/failure/error.md).
Raw LMDB behavior belongs to the [LMDB operation specification](../spec/storage/lmdb-operation.md), while operation-specific library failure behavior belongs to the [library specifications](../spec/library/README.md).
External-file recognition, parser containment, reusable container structure, and mapped-view lifetimes belong to the [encoded media architecture](encoded-media.md); exact reader behavior belongs to the [media file reading specification](../spec/media/file-reading.md).
Audio decoder translation belongs to the [decoder session specification](../spec/playback/decoder-session.md) and [decoder error reference](../reference/playback/decoder-error.md), not the library boundary.

## Implementation map

- [`MusicLibrary`](../../include/ao/library/MusicLibrary.h) owns the physical library environment and public read snapshots.
- [`WritableMusicLibrary`](../../include/ao/library/WritableMusicLibrary.h) owns explicit offline/live composition write authority and the process writer lease.
- [`WriteTransaction`](../../include/ao/library/WriteTransaction.h) owns native write lifetime, transaction-local dictionary interning, commit, rollback, and publication ordering.
- [`DictionaryStore`](../../include/ao/library/DictionaryStore.h) owns committed synchronized dictionary access, stable published values, generation, and bounded read contexts/caches.
- [`TrackStore`](../../include/ao/library/TrackStore.h) owns transaction-scoped point and ordered batch access to hot/cold track records.
- [`Library`](../../app/include/ao/rt/library/Library.h) composes the runtime reader, writer, task, and change roles.
- [`LibraryMutationService`](../../app/runtime/library/LibraryMutationService.h) owns live-runtime write admission, revision validation, commit, and publication completion.
- [`LibraryReader`](../../app/include/ao/rt/library/LibraryReader.h) and [`LibraryWriter`](../../app/include/ao/rt/library/LibraryWriter.h) define scoped read and synchronous mutation boundaries.
- [`LibraryTaskService`](../../app/include/ao/rt/library/LibraryTaskService.h) defines asynchronous library operations.
- [`LibraryChanges`](../../app/include/ao/rt/library/LibraryChanges.h) publishes revisioned changes and task status.
- [`TrackSourceCache`](../../app/include/ao/rt/source/TrackSourceCache.h) owns reusable sources and their dependency graph.
- [`LiveTrackListProjection`](../../app/include/ao/rt/projection/LiveTrackListProjection.h) is the primary ordered-list projection boundary.
- [`CoreRuntime.cpp`](../../app/runtime/CoreRuntime.cpp) is the ownership and lifetime composition root for the subsystem.

## Test map

- [`MusicLibraryTest.cpp`](../../test/unit/library/MusicLibraryTest.cpp) protects physical environment, store composition, cross-library transaction rejection, writer exclusion, and transaction-anchored lease lifetime.
- [`TrackStoreTest.cpp`](../../test/unit/library/TrackStoreTest.cpp) and [`TrackStoreRawLayoutTest.cpp`](../../test/unit/library/TrackStoreRawLayoutTest.cpp) protect batch order, missing-row behavior, and coordinated hot/cold traversal.
- [`DictionaryStoreTest.cpp`](../../test/unit/library/DictionaryStoreTest.cpp) protects overlay rollback, terminal commit-failure recovery, writer lifetime across transaction completion, stable borrowed views, bounded-cache behavior, batch binding, and all-or-none concurrent publication.
- [`PlanEvaluatorDictionaryTest.cpp`](../../test/unit/query/PlanEvaluatorDictionaryTest.cpp) protects bound dictionary predicates and explicit unresolved-symbol semantics.
- [`LibraryReaderTest.cpp`](../../test/unit/runtime/library/LibraryReaderTest.cpp) and [`LibraryWriterTest.cpp`](../../test/unit/runtime/library/LibraryWriterTest.cpp) protect runtime access roles.
- [`LibraryChangesTest.cpp`](../../test/unit/runtime/library/LibraryChangesTest.cpp) protects revision ordering and callback publication.
- [`LibraryAuthoringTest.cpp`](../../test/unit/runtime/library/LibraryAuthoringTest.cpp) protects availability, binding validation, all-or-none authoring, publication barriers, and terminal post-commit faults.
- [`LibraryTaskServiceTest.cpp`](../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) protects worker/callback task boundaries.
- [`TrackSourceCacheTest.cpp`](../../test/unit/runtime/source/TrackSourceCacheTest.cpp) protects source lifetime, reuse, and refresh composition.
- [`TrackListProjectionLifecycleTest.cpp`](../../test/unit/runtime/projection/TrackListProjectionLifecycleTest.cpp) and [`TrackListProjectionDeltaContractTest.cpp`](../../test/unit/runtime/projection/TrackListProjectionDeltaContractTest.cpp) protect the source-to-projection boundary.
- [`TrackListProjectionGroupingTest.cpp`](../../test/unit/runtime/projection/TrackListProjectionGroupingTest.cpp) protects normalized grouping keys and raw presentation labels.

## Related documents

- [System architecture](system-overview.md)
- [Runtime execution architecture](runtime-execution.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Encoded media architecture](encoded-media.md)
- [Resource delivery architecture](resource-delivery.md)
- [Track expression architecture](track-expression.md)
- [Presentation architecture](presentation.md)
- [Playback architecture](playback.md)
- [Workspace architecture](workspace.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Library specifications](../spec/library/README.md)
- [Library reference](../reference/library/README.md)
- [LMDB operation specification](../spec/storage/lmdb-operation.md)
- [Library YAML transfer specification](../spec/library/runtime/yaml-transfer.md) and [format reference](../reference/library/format/yaml.md)
- [Media file reading specification](../spec/media/file-reading.md) and [supported audio files reference](../reference/media/audio-file.md)
- [RFC 0022: transaction-coherent library dictionary](../rfc/0022-transaction-coherent-library-dictionary.md)
- [RFC 0023: revision-bound metadata authoring](../rfc/0023-revision-bound-metadata-authoring.md)
