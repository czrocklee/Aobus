---
id: architecture.library
---
# Library architecture

## Scope

This document explains ownership and dependency direction from physical music-library storage to runtime sources and live projections.
It identifies where read and write authority resides, how committed state becomes observable, and which owner keeps each borrowed value alive.

Exact records and compatibility rules belong to the [database reference](../../reference/library/storage/database.md), [Track model](../../reference/library/model/track.md), and [List model](../../reference/library/model/list.md).
Command behavior, publication ordering, scan reconciliation, transfer, source deltas, and projection deltas belong to the focused [library specifications](README.md).

## System context

The library pipeline is composed in two stages:

```text
CoreRuntime
  MusicLibrary storage and transactions
    -> Library snapshot | commands | jobs | changes
    -> TrackSourceCache and TrackSource implementations

AppRuntime
  CoreRuntime
    -> ViewService / playback consumers
    -> TrackListProjection / TrackDetailProjection
    -> workspace, playback, UIModel, and frontends
```

`CoreRuntime` owns one physical library, runtime facade, change bus, and source cache for one music root and database path.
CLI operations use this core composition without constructing interactive views or projections.
`AppRuntime` owns `CoreRuntime` and adds `ViewService`, workspace, playback, and projection consumers for interactive applications.

All stages remain in the application-runtime or core-library layers defined by the [system overview](../overview.md):

| Stage | Layer | Public boundary | Implementation |
|---|---|---|---|
| Physical storage | Core libraries | `include/ao/library/` | `lib/library/` |
| Runtime library facade | Application runtime | `app/include/ao/rt/library/` | `app/runtime/library/` |
| Track sources | Application runtime | `app/include/ao/rt/source/` | `app/runtime/source/` |
| Live projections | Application runtime | `app/include/ao/rt/projection/` | `app/runtime/projection/` |

UIModel and frontends consume runtime values and semantic commands; they do not join the storage or runtime-library implementation boundary.

## Physical storage and capabilities

`ao::library::MusicLibrary` owns the LMDB environment and the Track, List, Resource, dictionary, and file-manifest read services.
Its recoverable construction boundary is `MusicLibrary::open()`.
Open admits the schema and validates current-version local and cross-store invariants before exposing a usable library; it never exposes a partial store graph or partial All Tracks source.
The database combines scan-derived facts with library-only identities and user-authored curation, so rescanning media is not a reconstruction guarantee.
See the [database reference](../../reference/library/storage/database.md) for the exact catalog, layout, capacity, admission, and compatibility contracts.

A `ReadTransaction` owns one coherent native snapshot.
Store readers accept only transactions carrying the same stable `MusicLibrary` identity.
Views returned by those readers borrow mapped bytes and must not outlive the transaction; an intervening write may also invalidate a borrowed value inside a write transaction.
Application results copy or otherwise own everything they retain.
Dictionary values are the exception to ordinary record borrowing: committed dictionary entries are append-only and live in stable store-owned memory until the `DictionaryStore` is destroyed.
Each dictionary read or symbol-binding batch observes either the complete old mapping or the complete published mapping, never a partially installed transaction delta; separate calls do not form a multi-call snapshot.
Transaction-local interned entries remain hidden until commit succeeds. Abort or failed commit discards the unpublished tail, so its ids may be reused without rebinding any committed id.
Before exposing a new read transaction's Track views, `MusicLibrary` extends its in-memory dictionary with any committed tail visible in that native snapshot, including additions from another supported writer process.
Writer-capability acquisition refreshes that tail again after acquiring the writer-session lease, closing the gap between opening the library and writer admission.

Committing access requires a separately acquired `WritableMusicLibrary`.
Acquisition takes the non-blocking writer-session lease for the database path, and every transaction retains an anchor to that lease.
Destroying the originating capability therefore cannot release another process to write while one of its transactions remains alive. A transaction releases its lease anchor on commit, failure, abort, or destruction; the lease remains held while its originating capability still owns it.
`MusicLibrary` itself exposes no public write-transaction factory.

A `WriteTransaction` owns one native write transaction, the process writer gate, its lease anchor, transaction-local dictionary state, and lazily created physical writers.
After native begin acquires LMDB's single-writer snapshot, construction reads that snapshot's durable header and revision and computes its candidate revision before exposing the wrapper. Failure to begin or read those facts releases the incomplete transaction, process gate, and transaction lease anchor before the public factory fails fatally; the originating writable capability keeps its session lease.
Successive logical operations and successful `apply()` callbacks reuse each touched Store's physical writer and native cursor until commit or abort.
Every logical operation runs through the non-nested `WriteTransaction::apply()` root.
The callback receives a `LibraryWrite`, not the transaction owner:

- `LibraryWrite::tracks()` preserves Track, manifest, dictionary, and Resource relationships;
- `LibraryWrite::lists()` preserves live parent topology and deletion ordering; and
- `LibraryWrite::restoreLibraryIdentity()` is the narrow metadata mutation used by restore.

The callback-scoped capability cannot commit or abort.
Its logical writers must not escape the callback or outlive the transaction owner.
A returned error, a contained native/library mutation failure, or an unexpected exception aborts the complete root before it crosses the boundary.
There are no nested transactions, item savepoints, or supported continuation after a failed root.
`apply()` catches the private `lmdb::detail::TransactionFailure`, explicitly aborts and terminalizes the root, then returns its carried error. `commit()` contains mutation faults during dictionary preparation the same way. The carrier alone does not terminalize a native transaction; the root owner does.

Track preparation validates both record sides before its first item-relative dictionary, Resource, Track, or manifest effect.
Once preparation or writing has staged an effect, a later failure must reach the root boundary so the whole transaction rolls back.
Dictionary rows, Resource descriptors, Track records, manifest bindings, List records, an optional restored identity, and the candidate revision commit atomically when one operation composes them.

Physical Store writers and mutable LMDB reservations are library implementation details.
Production callers submit semantic builders to the logical writers rather than serialized records.
The source-private Track reservation path fills and validates one transaction-owned value synchronously and does not return mutable mapped storage to its caller.

`LibraryUri` is the shared music-root-relative path value used by storage, scanning, transfer, and file consumers.
It is canonical and separator-stable but deliberately not Unicode-normalized or case-folded.
Each actual file-access boundary resolves the URI below the weakly canonical music root and rejects escaping or unresolved symlinks.
This is a containment contract, not protection against an adversarial tree replacement between resolution and open.

## Runtime library facade

`ao::rt::Library` groups four public roles over the physical library:

- `LibrarySnapshot` owns one read transaction for a coherent read batch.
- `LibraryCommands` owns semantic mutations and exact previews.
- `LibraryJobs` owns scan, transfer, and identity work plus best-effort task progress.
- `LibraryChanges` exposes committed revision observation.

The private `LibraryWriteLane` owns the one live-runtime `WritableMusicLibrary` and command lane.
`Library::prepare()` acquires that authority and returns a move-only construction token which is consumed when `CoreRuntime` constructs the nonmovable facade in final storage.
The token is not a public runtime role.
The [session lifecycle](../session-lifecycle.md#interactive-runtime-composition) owns the surrounding composition and teardown order.

The lane serializes interactive commands, background write phases, and YAML-import Maintenance transitions.
A granted command runs its transaction in one synchronous worker-side kernel; no native transaction spans `co_await`.
An effective operation returns its value and exact zero-revision `LibraryChangeSet` together.
Only the coordinator may commit, stamp that changeset with the candidate revision, and submit it for publication.
A preview or `Unchanged` outcome aborts and publishes nothing.

Import holds explicit Maintenance while it prepares, previews, or applies its generation-bound operation.
Scan apply and audio-identity backfill instead hold a background-task lease during preparation and enter the same command lane only for their write phases.
They leave authoring availability open during preparation; later authoring reports non-terminal contention rather than waiting behind an outstanding background mutation.
The [task execution specification](task-execution.md) owns the complete admission, progress, cancellation, and workflow-settlement behavior.

Metadata, tag, combined Properties, and saved-order authoring require runtime-created bindings.
Bindings carry runtime identity, committed revision, and the exact target or effective order evidence needed by their command.
The active lane turn revalidates that evidence before mutation.
See [library access and mutation](mutation.md) for status precedence and command-specific behavior.

## Change publication and sources

A successful commit first publishes its dictionary and metadata cache state, then submits one revisioned changeset to `LibraryChanges`.
`TrackSourceCache` is the runtime's single bound publication replica.
It applies the committed revision before phase-two observers are notified, so an observer never learns a revision whose source state is still old.
The committing command retains its lane turn through replica application, observer delivery, and the applicable availability notification.
Task completion and frontend workflows never form a second refresh path.
The exact post-commit failure and Closing rules belong to [change publication](change-publication.md).

`TrackSource` is an ordered, observable identity sequence.
`CoreRuntime` owns `TrackSourceCache`, including All Tracks, saved-List source shells, predicate evaluators, saved-rank overlays, and weak-cached ad-hoc filters.
Callers acquire a `TrackSourceLease`; the lease pins the exact source and every upstream dependency needed by it.

Every saved List uses the same derivation:

```text
parent TrackSource
  -> SmartListSource(local expression; empty means true)
  -> ListOrderSource(raw saved rank)
  -> cached List source shell
```

The expression owns membership and the rank overlay owns effective order.
A hidden rank remains stored while its Track is outside membership and becomes visible again when membership returns.
Deleting a List invalidates its source identity and dependents terminally; recreating the same numeric id creates a new identity.
See [track sources](track-source.md) for cache rebinding, error propagation, and delta semantics.

## Projections and views

Live projections are interactive application consumers above `CoreRuntime`, not members of `CoreRuntime` itself.
`AppRuntime` constructs `ViewService` from the core source cache, physical read facade, change bus, and ordering policy.
`ViewService` owns open track-list projections; playback may own detached projections that capture the source and ordering policy at construction.
A later locale-policy update reaches open view projections, not an already detached playback conversation.
Track-detail projections also borrow `ViewService`, workspace, library reads, and `LibraryChanges`.

`TrackListProjection` combines a leased ordered source with a presentation shape.
It owns frontend-neutral row order, lookup indexes, group sections, and derived text keys.
Its raw dictionary-text cache borrows stable dictionary storage, while normalized keys borrow its own arena; rebuild releases all arena-backed holders before reclaiming the arena.
A locale-policy replacement is retained by shared ownership and rebuilds the affected projection.

`TrackDetailProjection` follows either workspace focus, one view, or an explicit selection and rebuilds owned snapshots from one read transaction.
Neither projection owns persistence authority or parses a saved List definition independently.
UIModel and frontends consume snapshots and commands rather than opening storage transactions to reconstruct them.

The [track-list projection](track-list-projection.md) and [track-detail projection](track-detail-projection.md) specifications own their refresh, delta, and lifetime contracts.
The [presentation architecture](../presentation/README.md) owns presentation state; [workspace](../workspace/README.md) owns view identity and navigation.

## Boundaries and dependency direction

- Core storage depends on lower library and LMDB facilities, never application runtime.
- Runtime library code may depend on `MusicLibrary`; normal application consumers use `ao::rt::Library` roles and owning values.
- Sources consume committed library state and changes; storage does not know sources exist.
- Query evaluation does not own source identity, leases, saved rank, or source deltas.
- Projections consume source leases and library reads; sources do not depend on projections.
- `CoreRuntime` owns storage, library roles, completion, and source state; `AppRuntime` adds views, workspace, playback, and projections.
- UIModel and normal frontends cannot name committing write authority or include direct storage implementation types.
- CLI inspection may use `CoreRuntime::musicLibrary()` for the documented low-level escape hatch, but that const access cannot create a write transaction.

The application architecture audit enforces the public include and capability boundaries; it does not replace the runtime transaction and lifetime contracts above.

## Data and control flow

A live mutation follows this route:

```text
owning command inputs
  -> LibraryWriteLane admission
  -> synchronous WriteTransaction::apply(LibraryWrite&)
  -> Unchanged(value) or Changed(value, zero-revision changeset)
  -> one native commit with candidate revision
  -> dictionary and metadata cache publication
  -> LibraryChanges replica application
  -> source deltas and live projections
  -> phase-two changed observers
  -> applicable availability observers
  -> worker-side command completion
```

A read-oriented workflow creates one `LibrarySnapshot`, performs related reads through its one transaction, and releases it before retaining application values.

A transfer uses a prepared authorization rather than a bare path:

```text
strict parse + prepared values + uncommitted preview
  -> LibraryImportPlan(source bytes, runtime, library id, revision, report)
  -> explicit authorization or drop
  -> source and target revalidation
  -> one atomic import commit and publication
```

A scan plan similarly carries one library id, planner provenance, and item-specific manifest evidence.
Slow file reads and hashing happen without a write transaction.
After the background command obtains the lane turn, it revalidates filesystem evidence before opening the transaction and admits all actionable database evidence before the first mutation.
See [scan and identity](scan-and-identity.md) for ordinary stale-item skips and the stricter moved-item rollback rule.

## Structural constraints

- One live `MusicLibrary` instance belongs to one database path in a process composition.
- Every exposed current-version store graph passed mandatory open-time validation.
- Every persisted Track has one hot row, one cold row, and one canonical manifest binding.
- Read and write capabilities are accepted only by the `MusicLibrary` that minted them.
- Write transactions are process-serialized and non-nested.
- Only `WritableMusicLibrary` can construct a write transaction; only the live coordinator owns that capability in a runtime.
- Every recoverable root-operation failure aborts the complete transaction before it is returned.
- An active transaction keeps the cross-process writer lease alive.
- A candidate revision is persisted with the content commit and is not consumed on abort or failed commit.
- A live committed revision becomes available only after ordered publication settlement.
- One replica applies a revision before notification observers receive it.
- Source caches and projections derive state from storage plus the ordered change stream; they are not persistence authorities.
- Saved rank affects ordering, never membership, and never All Tracks state.
- Borrowed Store views remain inside their transaction; projection-owned values remain inside their arena; source identity remains inside a lease.

## Failure, cancellation, and lifetime boundaries

Synchronous readers finish their snapshot scope before returning retained values.
Asynchronous writers hold no transaction across suspension and return only after abort/no-op or publication settlement.
Cancellation may stop only operations with explicit checkpoints and never reinterprets a durable commit as rolled back.

Persisted corruption safely detected during `MusicLibrary::open()` rejects the whole library as `CorruptData`; a valid non-current version returns `NotSupported` before current-schema interpretation.
After admission, a supported-writer invariant breach fails fast instead of becoming a miss or partial result.
Raw mapped-file faults may still terminate the process; no degraded-library or salvage-row contract exists.
A media rescan cannot preserve database-only curation, and a damaged database cannot be assumed exportable.

Before commit, recoverable storage and external-data failures use the operation's typed result channel and leave the previous committed state visible.
After a live durable commit, publication admission or delivery failure is fatal because the runtime cannot safely report an uncommitted result or continue with stale replicas.
Coordinated Closing is the sole retirement path: it seals command and task admission, retires queued work, requests stop from pre-transaction work, and waits for the lane and publication owner to quiesce.
A synchronous publication observer must defer shutdown to a later callback-executor turn.

`CoreRuntime::shutdown()` seals library mutation/publication before closing callback resumption, then stops and joins workers while library-backed collaborators remain alive.
`AppRuntime::shutdown()` first quiesces playback callback producers, then shuts down its owned core.
Source and projection subscriptions release before the owners they observe; projection caches release borrowed dictionary and arena views before `MusicLibrary` destruction.

## Implementation map

- [`MusicLibrary`](../../../include/ao/library/MusicLibrary.h), [`WritableMusicLibrary`](../../../include/ao/library/WritableMusicLibrary.h), [`WriteTransaction`](../../../include/ao/library/WriteTransaction.h), and [`LibraryWrite`](../../../include/ao/library/LibraryWrite.h) define storage and mutation capabilities.
- [`TrackWriter`](../../../include/ao/library/TrackWriter.h) and [`ListWriter`](../../../include/ao/library/ListWriter.h) own logical aggregate mutation.
- [`Library`](../../../app/include/ao/rt/library/Library.h), [`LibraryWriteLane`](../../../app/runtime/library/LibraryWriteLane.h), and [`LibraryChanges`](../../../app/include/ao/rt/library/LibraryChanges.h) define the live runtime boundary.
- [`TrackSourceCache`](../../../app/include/ao/rt/source/TrackSourceCache.h) owns reusable sources and publication-replica application.
- [`TrackListProjection`](../../../app/include/ao/rt/projection/TrackListProjection.h) and [`TrackDetailProjection`](../../../app/include/ao/rt/projection/TrackDetailProjection.h) define live projection values.
- [`CoreRuntime.cpp`](../../../app/runtime/CoreRuntime.cpp) composes storage, library roles, and sources; [`AppRuntime.cpp`](../../../app/runtime/AppRuntime.cpp) composes interactive views and playback above that core.

## Test map

- Storage tests under [`test/unit/library/`](../../../test/unit/library) protect open admission, transaction capabilities, rollback, writer exclusion, and logical aggregate writes.
- [`DictionaryStoreTest.cpp`](../../../test/unit/library/DictionaryStoreTest.cpp) protects complete dictionary publication, hidden uncommitted entries, failed-commit rollback, id reuse, and stable borrowed text; [`MetadataStoreTest.cpp`](../../../test/unit/library/MetadataStoreTest.cpp) protects external-tail refresh on read snapshots and writer admission.
- Runtime library tests under [`test/unit/runtime/library/`](../../../test/unit/runtime/library) protect command admission, publication settlement, scan/transfer behavior, and shutdown retirement.
- Source tests under [`test/unit/runtime/source/`](../../../test/unit/runtime/source) protect leases, cache identity, expression/rank composition, and deltas.
- Projection tests under [`test/unit/runtime/projection/`](../../../test/unit/runtime/projection) protect lifecycle, ordering, grouping, locale replacement, and incremental equivalence.

## Related documents

- [Library topic guide](README.md)
- [Library database reference](../../reference/library/storage/database.md)
- [Track model](../../reference/library/model/track.md) and [List model](../../reference/library/model/list.md)
- [Library access and mutation](mutation.md)
- [Library change publication](change-publication.md)
- [Library task execution](task-execution.md)
- [Track sources](track-source.md)
- [Track-list projection](track-list-projection.md) and [track-detail projection](track-detail-projection.md)
- [Library YAML transfer](yaml-transfer.md) and [YAML format](../../reference/library/format/yaml.md)
- [LMDB operation specification](../persistence/lmdb-operation.md)
- [Session lifecycle](../session-lifecycle.md)
