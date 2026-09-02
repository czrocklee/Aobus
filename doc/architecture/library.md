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
  snapshot | commands | jobs | changes
            |
            v
TrackSourceCache + TrackSource implementations
            |
            v
TrackListProjection / TrackDetailProjection
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
Process composition keeps only one live `MusicLibrary` for a database path because LMDB forbids opening the same database twice in one process; the library does not maintain a canonical-path registry or turn that upstream precondition into `Conflict`.
It creates public read transactions and owns cached logical metadata-header and committed-revision state; the physical metadata Store is private.
Its sole public construction boundary is `MusicLibrary::open()`, which returns a typed result, enumerates the LMDB catalog before creation, and validates the current schema, metadata, dictionary, Resources, paired Track records and references, List graph, and Track-to-manifest bijection before exposing the store graph.
That unpublished initialization write transaction first admits the main catalog as byte-key storage, then opens each of the seven named DBIs exactly once and retains typed integer-key or byte-key tokens for the library lifetime; normal reads never reopen a DBI.
On existing storage, one source-private read-only `meta` token remains unpublished while the stable version prefix is inspected; a current version consumes that same DBI into an integer-key token after exact named-database flag validation, while a non-current version returns before current-schema interpretation.
Safely detected corruption in those records rejects the complete open; runtime composition cannot receive a partial library or build a partial All Tracks source.
The environment combines user-authored library truth with scan-derived facts; media rescan cannot reconstruct the complete database.
Committing writes require a separately acquired `WritableMusicLibrary`; `MusicLibrary` keeps transaction construction private to that capability.
Acquisition takes a non-blocking OS file lease for the database path, so a second writable process receives `Conflict` while the first capability or any transaction anchored to it remains active.
The capability borrows its `MusicLibrary`; storage composition keeps that library alive until the capability and all transactions anchored to its lease are destroyed.
Read-only processes do not take that lease.
The core [LMDB operation specification](../spec/storage/lmdb-operation.md) owns environment, transaction, cursor, and raw read/write behavior below these library-specific stores.

Every `MusicLibrary` read uses one move-only `ReadTransaction` that directly owns a native LMDB read transaction.
The wrapper is the library-level snapshot capability: store readers accept it, while its native handle remains private to `MusicLibrary` and the stores.
Borrowed `string_view` values returned by `CustomMetadataProxy::get()` alias the selected cold-record bytes and cannot cross transaction completion, backing mutation, reentrant work, or coroutine suspension without an explicit owning copy and a separately proved storage lifetime.
The wrapper and every store carry the same stable implementation-owned library identity, so a snapshot from one `MusicLibrary` is rejected before it can be mixed with another library's DBI.
This adds no allocation, locking, or another transaction layer to each operation.

Every writable-capability write uses one move-only `WriteTransaction` that owns the native LMDB transaction, the process writer gate, a shared writer-lease anchor, and the transaction-local dictionary overlay.
After native begin acquires LMDB's single-writer snapshot, creating the wrapper reads that snapshot's durable header and revision and computes its one candidate successor without persisting it.
A native begin failure or revision exhaustion releases the process writer gate and discards the transaction's lease-anchor reference, then fails through the fatal facility; transaction construction is not a recoverable authoring outcome.
Every logical write body runs through `WriteTransaction::apply()`; an error result, private native mutation marker, or private library error carrier explicitly aborts the complete root before returning the error, while an unexpected exception aborts before it is rethrown.
`apply()` is a non-nested root boundary and passes its callback a narrow `LibraryWrite` operation context rather than the transaction owner.
Only that context exposes logical Track and List mutation ports plus the narrow library-identity restore operation; it exposes neither commit nor abort and exists only for the callback invocation.
Logical writers returned by the context borrow the transaction owner, are valid only while that callback is active, and must not outlive the transaction object; retaining the context itself beyond the callback is an ordinary dangling-reference error rather than a wider lifetime guarantee.
A successful body leaves the root active for a later explicit commit by its transaction owner.
Track preparation owns text admission, dictionary interning, and descriptor creation: user-visible text is validated and normalized to NFC, interning first consults committed canonical mappings and then the transaction overlay, and new id/text or resource descriptor rows are written into the same native transaction as the Track record that references them.
Dropping or failing the wrapper aborts both authorities.
Commit or abort consumes the native handle but retains the native transaction object and dictionary writer until the outer wrapper is destroyed.
Store writers that remain in ordinary scope across `commit()` therefore observe a terminal transaction and can be destroyed safely; any post-terminal operation fails before touching an LMDB cursor.

Before native commit, the wrapper prepares every potentially throwing dictionary insertion, acquires the logical metadata publication lock, persists any candidate header and then the candidate revision, and enters native commit.
It holds the publication locks through commit, then either publishes the dictionary, header, and committed revision together or rolls the prepared in-memory state back before unlocking.
Readers therefore observe either the complete old mapping or the complete new mapping, and application change publication happens only after the latter is visible.

Committed dictionary ids form a dense, append-only range beginning at one; aborted tail ids may be reused, while committed ids are never reclaimed or rebound.
Opening builds the in-memory dictionary indices and establishes the persisted key-order, scalar-valid UTF-8 NFC, and canonical value-uniqueness invariants in the same mandatory traversal rather than validating a second copy of the strings first.
`DictionaryStore` serializes committed index publication and lookup with its internal shared mutex.
Published strings are immutable and use stable storage, so a borrowed view remains valid until the store is destroyed even when later commits grow the dictionary.
`DictionaryReadCache` is a bounded, owner-thread batch accelerator over those views rather than a snapshot or a wider lock scope.
Its collision replacement may cause another store lookup but cannot change a result; empty values are simply not retained.
`DictionaryReadContext` is the bounded synchronous read/binding port used by query and format evaluation.

Store types own physical representation and transaction-scoped read access.
`MusicLibrary` exposes stores as const read service handles; read capability comes from `ReadTransaction` or `WriteTransaction`.
Mutation is available only through the `LibraryWrite` operation context supplied by `WriteTransaction::apply()`.
The Track writer owns dictionary/resource preparation plus every Track/manifest relationship; the List writer owns live parent topology and deletion ordering.
Physical Store writer factories are implementation details, with one source-private access seam limited to representation, corruption, and isolated Store-backed tests.
Raw LMDB transactions are not part of the public store operation surface.
They do not publish application events or construct frontend projections.

`TrackStore` owns both point reads and ordered batch reads of track records.
Its batch boundary preserves the caller's requested ID order, skips missing
rows, and retains duplicate requests. It chooses between point lookup and
coordinated cursor traversal internally, so predicate evaluators and
projections do not duplicate LMDB access policy.
`TrackStore::Reader` is itself the complete hot-and-cold input range, while its `hot()` and `cold()` projections expose one physical side for intentional tier-specific traversal; point and ordered batch reads retain explicit load selection.
Its `entryCount()` reports complete logical Track rows and rechecks the paired hot/cold row-count invariant.
The open-time integrity gate proves that the hot and cold key sets match; combined traversal then advances both cursors in lockstep and treats any later divergence as an internal invariant failure rather than skipping an orphan.
Every record mutation uses an immutable prepared value and never exposes caller-supplied record bytes or mutable LMDB storage.
Track encoding fills and validates one callback-scoped reservation at a time through the source-private `lmdb::detail::ReservationWriterAccess`; public LMDB create, update, and append operations accept only copied input bytes.
List and manifest writes pass their already-owned encoded bytes through that ordinary copied-data path.
The logical Track writer is the only production owner of Track preparation and physical Track, manifest, dictionary, and resource mutation.
Its public operations accept semantic `TrackBuilder` and `FileManifestBuilder` inputs, validate any caller-supplied existing Resource ids against the live write snapshot, and create transaction-bound prepared snapshots internally.
The transaction lazily opens at most one physical writer for each touched Store and reuses its native cursor until that transaction terminates, including across successive successful `apply()` callbacks.
Creation writes the hot/cold pair and its manifest binding together; deletion and clear remove both sides; a normal update must retain its URI; relink is the sole URI-changing operation and replaces the manifest key in the same transaction.
These operations preserve the relationship for accepted writes, and the open gate proves the historical Track-to-manifest bijection before any logical mutation is admitted.
A later missing or mismatched binding is therefore an invariant failure rather than damaged input discovered lazily.
Manifest-only status and audio-identity updates identify the owning Track and pass only mutable manifest facts; the logical writer derives the URI and Track binding from the live aggregate.
Paired create/update operations abort the complete library transaction when a later physical step fails, so a caller cannot commit one side of an application-level Track change by swallowing that error.
Complete Track preparation runs pure hot and cold preflight before dictionary interning, cover-resource creation, or record mutation.
That preflight validates text, computes post-NFC sizes, and rejects canonically duplicate custom keys; common already-NFC input uses a quick check to avoid a temporary normalized allocation.
The corresponding item is neutral relative to its entry transaction state when preflight rejects it; after its first staged effect, a failure must reach the root operation boundary, which aborts the complete transaction before exposing the error.
Prepared Track sides retain typed snapshots rather than a second encoded copy, so the canonical byte validators run after the zero-copy encoder fills storage and again during open admission.

`FileManifestStore` applies one exact validator at its point-read, iterator, prepared-write, and open-validation boundaries.
`FileManifestBuilder::validate()` parses the manifest URI and applies every key and record fact that does not depend on the owning Track id, which a creating write only allocates once its Track record exists.
Its unbound result always carries a zero Track id, so no caller-supplied or stale builder binding can reach storage.
`FileManifestBuilder::Unbound::bind()` then supplies the real nonzero id, cannot fail, and reapplies the complete record validator as its postcondition; the Store writer accepts only that bound value.
The writer passes the prepared owning bytes through the ordinary copied-data path; the same validator already established their canonical representation before mutation.
Only `NotFound` denotes absence at a point read.
A malformed manifest point-read or iterator row violates the already-established open invariant and fails fast instead of becoming a skippable item or partial output.
`ListBuilder::prepare()` similarly validates List text, normalizes display fields while preserving opaque filter bytes, then snapshots one canonical List record with bounded field sizes and nonzero unique saved-order ids.
The logical List writer accepts the semantic `ListBuilder`, performs preparation internally, and never exposes prepared bytes as a logical mutation input.
The Store writes that exact immutable snapshot through the ordinary copied-data path, avoiding both a second serialization pass and a second allocating saved-order validation pass.
The local validator requires scalar-valid UTF-8 filter text but preserves its bytes and treats its syntax as opaque; parent existence and cycle checks are cross-row logical-writer responsibilities rather than record facts.
The logical List writer performs those cross-row checks against the live write snapshot, rejects ordinary deletion while children exist, and performs explicit subtree deletion children-first.
`ListStore` otherwise follows the same validated-iterator rule, and its optional point reads abort through `AO_INVARIANT` on a post-open structural breach because `nullopt` means absence only.

`LibraryUri` is the shared core value for paths in the music-root namespace.
YAML import, runtime Writer, scanner, `FileManifestStore`, and runtime file consumers converge on this value instead of maintaining independent path checks.
Its bounded canonical text is root-relative and separator-stable; each actual access resolves it beneath the root so an escaping or unresolved symlink is rejected at that boundary.
It remains outside user-visible text normalization: NFC admission never rewrites filesystem identity.
This narrows path races but is not an adversarial filesystem sandbox: the music tree is assumed not to change between resolution and the operating-system open.

### Runtime library facade

`ao::rt::Library` is the application access boundary over `MusicLibrary`.
It exposes four cooperating roles and owns one private mutation coordinator:

- `LibrarySnapshot` owns one read transaction for a coherent point-in-time read batch.
- `LibraryCommands` owns asynchronous semantic commands and exact previews; every effective command commits and publishes through the coordinator.
- `LibraryJobs` owns long-running asynchronous operations such as scan, import/export, and identity backfill, including best-effort progress and its status-free presentation finalization pulse.
- `LibraryChanges` is the read-only committed-revision observation boundary.
- `LibraryWriteLane` exclusively owns the writable core capability and the one live-runtime command lane, including interactive/background/Maintenance admission, commit revision checks, publication settlement, and Closing quiescence.

`LibraryChanges` keeps its publication state machine in one unique owner object.
Only a narrow shared delivery/admission block survives queued callback dispatch or a late replica-subscription reset, and that block exists only to weakly enter the owner, retain already-admitted delivery stacks, and preserve delivery diagnostics until those stacks retire.

The facade borrows storage, async runtime, and change-bus collaborators owned by `CoreRuntime`.
`CoreRuntime` allocates and finalizes its `Impl` and direct `MusicLibrary` first, then `Library::prepare()` acquires `WritableMusicLibrary` against that final object and returns a short-lived move-only `Library::Prepared` construction token.
The token carries only that successfully acquired authority across the typed-error boundary and is consumed when `CoreRuntime` emplaces the nonmovable `Library` directly in phase-local optional storage; it is never published as a runtime role.
`CoreRuntime` and `AppRuntime` wrappers remain move-only PImpl values; moving either wrapper transfers only its PImpl and moved-from destruction is inert.
It groups roles and lifetime; the coordinator is an application control plane over the existing LMDB transaction system rather than another database or nested transaction layer.

Lane admission uses move-only value permits rather than heap-allocated lease wrappers.
A narrow owner permit increments the active-call count and a command guard owns that permit plus the granted request through every pre-transaction or Maintenance control-delivery path.
Mutation construction transfers only the request, lifetime state, and raw owner after the write transaction has constructed successfully; terminal mutation release calls `releaseCommand()` before releasing the active-call permit.
Shared request, one-shot, publication, and lifetime states remain shared because their coroutine, queue, and owner lifetimes can end independently.

Library import has a prepared capability boundary rather than a path-based commit command.
`LibraryJobs` produces a move-only `LibraryImportPlan` after strict parsing and an uncommitted preview, binding it to exact source bytes plus the target runtime, library identity, and committed revision.
Applying consumes that plan once and revalidates every binding before opening the committing mutation.
Frontends may present the report or drop the plan, but cannot manufacture or retarget commit evidence.

The coordinator publishes `LibraryAuthoringAvailability` as `Available` or `Maintenance`.
Maintenance identifies YAML import and rejects every interactive command for the whole operation, including slow preparation outside transaction ownership.
Scan apply and audio-identity backfill instead hold a non-presentational background-task lease that serializes them with each other and with import preparation while leaving authoring `Available` during preparation.
Their write phases enter the same FIFO lane as interactive commands; once one background mutation is queued or active, later interactive authoring reports non-terminal `Busy` and ordinary mutation/preview commands report `ResourceBusy` rather than blocking an executor thread.
An interactive command accepted first retains its lane position.

Queue waits and publication-settlement waits suspend their coroutines.
After a command receives the active turn, it resumes on a worker and runs one synchronous, non-coroutine transaction kernel; the command retains the turn through `Published` or coordinated-Closing retirement.
Maintenance entry and exit are themselves sequenced control commands, and availability delivery runs on the serialized callback executor without the coordinator state mutex.
Mutation attempted from a publication or availability observer is rejected as reentrant rather than queued for a later causal turn.
The [task execution specification](../spec/library/runtime/task-execution.md#background-tasks-and-maintenance-states) and [change publication specification](../spec/library/runtime/change-publication.md#ordering-and-delivery) own the exact admission and settlement behavior.
Closing is private lifetime coordination rather than a public authoring state.
Metadata, tag, and combined Properties authoring additionally requires runtime-created `BoundTrackTargets` containing the runtime instance id, committed library revision, and exact target order.

### Sources

`TrackSource` is the runtime boundary for an ordered, observable set of track identities.
`TrackSourceCache` owns the All Tracks source, cached saved-List sources, predicate evaluation, saved-rank overlays, dependency links between Lists, and reusable ad-hoc filtered sources.
Every saved List has the same composition:

```text
parent TrackSource
  -> SmartListSource(local expression; empty means true)
  -> ListOrderSource(raw saved rank)
  -> cached List source
```

The predicate owns membership and `ListOrderSource` owns only effective order.
Ranked current members appear first in raw-rank order, unranked current members follow in parent order, and raw ranks for current nonmembers remain hidden until membership returns or an explicit command forgets them.
All Tracks is the permanent upstream source and has no writable rank overlay.

One `SmartListEvaluator` bucket rebuild creates one batch-local dictionary read cache/context, binds each immutable plan once, and shares those bindings across the tracks evaluated in that batch.
Binding resolves all plan symbols under one shared dictionary lock; later id-to-text cache misses take bounded point-read locks rather than delaying dictionary writers for a whole scan.
Metadata-only batches use a touched-track path that updates installed membership without rebuilding unaffected bucket indices; structural batches retain the general rebuild path.

Callers acquire leases rather than taking raw ownership of cached sources.
The cache observes `LibraryChanges` and turns committed storage changes into source refreshes or incremental source deltas.
That revisioned stream is the sole authoritative path from committed library facts to runtime replicas, sources, projections, and frontend cache invalidation; task completion and workflow callbacks do not form a second refresh path.

### Projections and views

Live projections combine a source lease with library reads and presentation structure.
They own frontend-neutral row/detail snapshots and publish projection deltas to consumers such as `ViewService` and the internal `PlaybackSuccession` owner.

`TrackListProjection` resolves each dictionary ID into one cached pair: raw presentation text borrowed from `DictionaryStore` and a normalized sort/group key owned by its `StringArena`.
The cache is projection-local and owner-thread confined; a full rebuild releases every dependent row and section view, clears the cache, and then reclaims the arena.

`ViewService` owns the lifecycle of open runtime views and their projections.
Its track-list state keeps the content axis (`listId` plus a transient `filterExpression`) separate from the shape axis (`TrackPresentationSpec`).
The [track expression architecture](track-expression.md) owns how expression text reaches a predicate-backed source, while the [presentation architecture](presentation.md) owns how presentation state reaches UIModel and frontends.
UIModel and frontends consume runtime snapshots and commands rather than opening storage transactions to reconstruct the same view independently.

## Boundaries and dependency direction

- Physical stores depend only on lower library/LMDB facilities and do not depend on runtime.
- Runtime library implementations may depend on `MusicLibrary`, but public application consumers use `ao::rt::Library` roles and value types.
- Sources consume committed library state and change events; storage does not know that sources exist.
- Predicate-backed saved and ad-hoc sources consume the core expression system, but the expression system does not own source identity, saved rank, leases, or deltas.
- Projections consume source leases and library reads; sources do not depend on projections or frontends.
- A projection combines source membership with presentation structure without allowing either concern to redefine the other.
- View, workspace, completion, and playback services consume sources/projections through runtime-owned boundaries.
- UIModel and normal frontend adapters do not include LMDB or concrete library store/view headers and cannot name committing write authority.

`CoreRuntime::musicLibrary()` is const and supports read-only CLI inspection plus internal runtime composition.
`AppRuntime` deliberately does not forward it, and frontend guardrails reject direct `CoreRuntime` dependencies.
It cannot create a library write transaction.
The check-owned `aobus_guardrails` target rejects write-transaction, writable-capability, and direct `LibraryCommands` dependencies from UIModel, GTK, and TUI; normal frontend mutation must cross UIModel or another semantic runtime command.

`CoreRuntime::create()` and `AppRuntime::create()` are the recoverable composition boundaries.
They return move-only values and typed errors from `MusicLibrary::open()` and writable-facade acquisition without a throwing compatibility constructor.
Before either factory exposes a runtime, `CoreRuntime` performs the initial complete All Tracks source reload from the validated database; interactive services are added only after that core initialization succeeds.

## Data and control flow

An asynchronous live mutation follows this path:

```text
callback/CLI command captures owning inputs
  -> LibraryCommands Task
  -> LibraryWriteLane FIFO admission
  -> active worker turn
  -> synchronous WriteTransaction + transaction-local dictionary overlay
  -> root execute boundary + callback-scoped LibraryWrite ports
  -> operation-owned Changed(value, LibraryChangeSet) or Unchanged(value)
  -> one LMDB commit with records, dictionary rows, and new library revision
  -> complete dictionary-index publication
  -> ordered LibraryChanges publication on the callback executor
  -> TrackSourceCache refresh/delta
  -> live projections
  -> view/playback/UI observers
  -> Available(runtimeInstanceId, committedRevision)
  -> publication event resumes the command on a worker
  -> caller receives the owned result on its executor
```

`Unchanged`, preview, and pre-commit failure paths abort the native transaction and release the lane without publication.
The caller continuation is not part of lane ownership.

An asynchronous mutating task acquires its task-level exclusion before it leaves the callback executor, performs slow preparation through `LibraryJobs` on the async worker pool without transaction ownership, and submits a coordinator command only for preview or apply/commit.
Import uses exclusive maintenance plus the background-task lease; scan apply and identity backfill use only the lease and therefore permit unrelated interactive authoring during preparation.
Export and scan-plan construction remain independent read snapshots.
After successful cancellable callback-owner admission, best-effort progress and a status-free finished pulse return through `LibraryJobs`, while the awaited task owns its outcome and committed content changes use `LibraryChangeSet` exclusively; the [task execution specification](../spec/library/runtime/task-execution.md#progress-and-outcome) owns the exact conversation boundary.

A library transfer follows a two-operation path:

```text
strict parse + prepared data + uncommitted preview
  -> LibraryImportPlan(report, source bytes, target runtime/id/revision)
  -> frontend authorization or drop
  -> binding revalidation
  -> one atomic import commit + LibraryChanges publication
```

A scan plan is an opaque move-only runtime value whose immutable items are bound to the persisted library id and carry planner revision plus per-item manifest evidence from one short read snapshot.
Planning owns a copy of that manifest state and releases the LMDB snapshot before filesystem traversal and identity hashing.
Scan apply validates the library id before preparation and at its single write boundary, revalidates filesystem evidence after its command receives the active turn but before opening the transaction, and admits each actionable item against live manifest and Track evidence before the first mutation.
Unrelated revisions do not invalidate a plan; stale ordinary items are counted separately from failures and skipped, while stale move evidence aborts the complete apply.
Scan apply crosses one coordinator mutation boundary; the [scan specification](../spec/library/runtime/scan-and-identity.md#plan-application) owns its item admission and atomicity behavior.
Explicit relink is a constrained plan derivation that preserves the same source evidence rather than a separate caller-authored mutation description.

A read-oriented workflow obtains one `LibrarySnapshot`, performs the related reads under its single transaction snapshot, and releases the snapshot before retaining application values.

Metadata, tag, and combined Properties authoring first binds the exact targets to one runtime instance and one available committed revision.
Commit rechecks runtime identity, availability, revision, and every target inside the command's active transaction turn.
A foreign or superseded binding is `Stale`, maintenance is `Unavailable`, lane contention is non-terminal `Busy`, and an effective commit returns a binding advanced to the published revision.
Creating a binding validates that every target exists and returns `NotFound` otherwise; disappearance under an accepted exact-revision binding is an invariant violation rather than another authoring status.

Saved-order authoring uses the parallel `BoundListOrder` evidence shape: runtime instance, committed revision, List id, and complete effective TrackId sequence.
The writer uses committed revision as transaction authority and stable TrackIds as movement operands; a frontend source/view generation is only an earlier gesture-cancellation signal and cannot prove a write transaction current.
Maintenance is an explicit `Unavailable` interval rather than a stream of stale revisions.

A filtered runtime view follows a separate composition path:

```text
base ListId + filter expression
  -> TrackSourceCache
  -> base or ad-hoc TrackSource
  -> TrackListProjection + TrackPresentationSpec
  -> ViewService observers
```

Changing the filter replaces the active source/projection resources while retaining presentation state.
Changing presentation reshapes the projection without changing base-list or filter identity.

A saved List first composes parent membership, its local predicate, and its raw rank before any transient view filter or presentation:

```text
parent source + local expression + raw saved rank
  -> effective saved-List source
  -> optional transient filter
  -> presentation sort/group
  -> projection
```

An empty presentation sort preserves the effective source order.
A nonempty sort controls projected and playback order without rewriting saved rank.

## Structural constraints

- One `MusicLibrary` instance and its runtime facade belong to one `CoreRuntime` and one music root.
- One persisted Track identity has exactly one canonical hot record and one canonical cold record.
- Every runtime-visible current-schema catalog entry, metadata record, dictionary and Resource row, Track pair and reference, List graph edge, and manifest binding has passed the open-time integrity gate.
- An unvalidated metadata DBI exists only inside unpublished `MusicLibrary` initialization and cannot escape as a Store token or be reopened during classification.
- Every persisted Track has exactly one manifest row at its canonical URI, that row names the same Track id, and no extra manifest row exists.
- One prepared Track write reaches mutable reservation storage only through the source-private reservation access and fills and validates each value inside its synchronous no-throw encoder invocation; reservation pointers never cross that callback boundary.
- A scan plan can mutate only its captured library id and items admitted against their transaction-current manifest and Track evidence.
- A library import plan can mutate only its captured runtime, library identity, committed revision, and exact source-byte snapshot, and applying it consumes the plan.
- Manifest keys and runtime-created track URIs are canonical `LibraryUri` values; every file-access boundary re-resolves them beneath the music root.
- A library transaction is accepted only by stores carrying the same stable `MusicLibrary` identity.
- Library write transactions are process-serialized and non-nested; dictionary mappings are append-only within one open library.
- A recoverable error from a root write body aborts the complete transaction before it is returned; the current library layer has no item-local savepoint or continuation after a failed body.
- One OS lease excludes another writable process, and an active transaction retains that lease even if its originating capability is destroyed.
- Live-runtime commits can begin only through the one coordinator-owned writable capability.
- A live committing operation returns its owning value and exact zero-revision changeset together; only the coordinator execute boundary may commit and stamp that payload.
- A noncommitting apply cannot later be upgraded to execute on the same mutation.
- A write transaction exposes one in-memory candidate successor revision, persists it immediately before native commit, and does not consume it on abort or failed commit.
- A mutation becomes observable through the revisioned change bus only after its write transaction commits and the cached metadata state publishes that candidate.
- The sequencer grants only one active live-runtime command; an effective command retains that turn through revision settlement, and callback-thread reentrant mutation during publication is rejected.
- Availability observers execute without coordinator mutex ownership, while the active publication or Maintenance control-delivery phase keeps the next command closed until synchronous delivery completes.
- One replica applies a revision before it is announced; notification observers only learn of revisions that replica already applied.
- Task-progress finalization and frontend workflow callbacks never refresh committed data; consumers derive such refresh only from `LibraryChanges`.
- Consumers use published track and list identities to refresh state; they do not retain transaction-bound core views beyond their scope.
- `LibraryChanges` accepts only the coordinator's exact successor revision and completes that publication before another commit can be admitted.
- `LibraryChanges` shares only that narrow delivery/admission block; it is not an independent publication owner, and its borrowed callback executor remains the lifetime ceiling for queued delivery work.
- Source caches and projections derive state from storage plus the ordered change stream; they are not independent persistence authorities.
- Saved rank is a persistence overlay on a saved List, never membership and never All Tracks state.
- Cached list sources retain stable identity until deletion or cache teardown; a lease keeps its exact source and
  upstream dependencies alive beyond cache teardown. Ad-hoc filtered sources remain weak-cached while leased, and acquiring a new ad-hoc identity prunes expired weak entries.
- Dictionary read caches never extend a store view beyond the owning `MusicLibrary`, and they do not provide transaction isolation or a dictionary snapshot.
- Projection raw-text caches borrow stable dictionary storage, while normalized projection keys never outlive the projection arena that owns them.
- Exact persistence records and exact delta operations are delegated to reference and specification documents.

## Failure, cancellation, and lifetime boundaries

Synchronous readers finish their snapshot scope before returning application values.
Asynchronous writers run their transaction scope entirely inside one worker-side synchronous kernel and return only after the command has aborted or reached revision settlement.
`LibraryJobs` owns the worker/callback transition for long-running operations and accepts cooperative stop tokens.
Executor hops honor cancellation, while only operations with explicit synchronous checkpoints can stop during their core work; [library task execution](../spec/library/runtime/task-execution.md#cancellation) owns the operation matrix.
Cancellation never reinterprets an already committed transaction as uncommitted.

Failure before commit returns through the operation's typed error channel and leaves the prior availability intact.
LMDB mutation faults may use a private `lmdb::detail::TransactionFailure` as short-range unwind control below the library wrapper.
`WriteTransaction::apply()` and `commit()` are the exact lower containment owners: they explicitly abort and terminalize the root before translating the carried `Error` to `Result`, and no runtime writer, task, scan, or importer catches the marker.
`LibraryWriteLane::Mutation::apply()` and `executeAsync()` additionally terminalize the live mutation and release its sequencer turn before returning an error or rethrowing an unexpected exception.
Command admission remains under a value guard until transaction construction succeeds, so cancellation, a rejected pre-transaction step, allocation failure while constructing the transaction, or any other exception releases the command first and then the active-call permit without stranding Closing.
Moved-from permits and mutations are inactive and have no teardown effect.
`executeAsync()` is the only live commit boundary; `Unchanged` and preview `apply()` paths explicitly abort and cannot publish.
No failed root can be continued or committed, even while its C++ wrapper remains alive.
Once durable commit succeeds, revision-admission, publication admission, or delivery failure in a live runtime is an infrastructure fault and terminates the process.
It is not translated to a transaction `Result` or a recoverable authoring state; the next process open reconstructs runtime state from the durable database.
Coordinated Closing is the only exception: it seals command and task admission, retires queued commands, requests stop from active pre-transaction work, and may retire a queued publication or unclaimed Maintenance control delivery before callback admission closes.
It does not stop the runtime until the command lane is idle and `LibraryChanges` is sealed.
`MusicLibrary::readTransaction()` and `WritableMusicLibrary::writeTransaction()` treat native begin failure as fatal because a live library has no alternate storage authority.
Malformed current-schema catalog, metadata, dictionary, Resource, Track, List, or manifest state that can be inspected safely before exposure rejects `MusicLibrary::open()` with `CorruptData`; a valid non-current metadata version returns `NotSupported` before current-schema closure.
A later Store structural breach is an `AO_INVARIANT` failure, and a later non-miss native read fault is `AO_FATAL`, because supported writers preserve the validated facts and no safe partial-result or degraded-library contract exists.
The same native read fault inside an active write transaction uses the private transaction carrier so the root owner aborts every staged effect before exposing a typed operation error.
Arbitrary LMDB or mapped-file corruption may still terminate the process; the storage contract prevents Aobus-authored partial records but does not promise row salvage, degraded operation, or in-process repair for a physically damaged database.
Media rescan does not preserve database-only curation, and a damaged database cannot be assumed exportable; recovery preserves that state only when a usable export or other backup already exists.

`CoreRuntime::shutdown()` first seals library mutation and publication admission, then closes callback resumption, stops and joins worker tasks while library-backed collaborators still exist.
A synchronous library observer must not run runtime shutdown or destroy the library on the same callback stack and must defer teardown to a later callback-executor turn.
`LibraryChanges` destruction waits for already-admitted delivery callbacks to retire, so a queued callback after retirement becomes a safe no-op; that retire-wait itself must not start from the same callback-executor delivery stack.
`AppRuntime::shutdown()` quiesces playback-session and audio callback producers before shutting down its owned core boundary.
Subscriptions held by sources and projections release before the `LibraryChanges` owner they observe.
Batch and projection dictionary caches are destroyed before the `MusicLibrary` that owns their borrowed raw views.

Recoverable storage and external-data failures cross the runtime facade as typed results.
Shared channel behavior belongs to the [outcome channel specification](../spec/failure/outcome-channel.md), and exact common codes belong to the [error value reference](../reference/failure/error.md).
Raw LMDB behavior belongs to the [LMDB operation specification](../spec/storage/lmdb-operation.md), while operation-specific library failure behavior belongs to the [library specifications](../spec/library/README.md).
External-file recognition, parser containment, reusable container structure, and mapped-view lifetimes belong to the [encoded media architecture](encoded-media.md); exact reader behavior belongs to the [media file reading specification](../spec/media/file-reading.md).
Audio decoder translation belongs to the [decoder session specification](../spec/playback/decoder-session.md) and [decoder error reference](../reference/playback/decoder-error.md), not the library boundary.

## Implementation map

- [`MusicLibrary`](../../include/ao/library/MusicLibrary.h) owns the physical library environment, typed retained DBI tokens, private metadata admission, and public read snapshots.
- [`WritableMusicLibrary`](../../include/ao/library/WritableMusicLibrary.h) owns explicit offline/live composition write authority and the process writer lease.
- [`WriteTransaction`](../../include/ao/library/WriteTransaction.h) owns native write lifetime, root-operation failure containment, transaction-local dictionary interning, commit, rollback, and publication ordering.
- [`LibraryWrite`](../../include/ao/library/LibraryWrite.h) is the callback-scoped logical mutation capability supplied only by `WriteTransaction::apply()`.
- [`TrackWriter`](../../include/ao/library/TrackWriter.h) owns logical Track/manifest mutation and transaction-local Track preparation.
- [`TrackWrite.cpp`](../../lib/library/TrackWrite.cpp) and [`ReservationWriterAccess.h`](../../lib/lmdb/detail/ReservationWriterAccess.h) own the sole production path from prepared hot/cold Track values to callback-scoped LMDB reservations.
- [`ListWriter`](../../include/ao/library/ListWriter.h) owns logical List topology mutation and deletion ordering.
- [`DictionaryStore`](../../include/ao/library/DictionaryStore.h) owns committed synchronized dictionary access, stable published values, generation, and bounded read contexts/caches.
- [`TextAdmission.cpp`](../../lib/library/TextAdmission.cpp) owns the library-private translation from reusable Unicode operations to mutation and corruption semantics.
- [`TrackStore`](../../include/ao/library/TrackStore.h) owns transaction-scoped point and ordered batch access to hot/cold track records.
- [`LibraryUri`](../../include/ao/library/LibraryUri.h) owns the canonical music-root-relative path namespace and resolved containment check.
- [`Library`](../../app/include/ao/rt/library/Library.h) composes the runtime reader, writer, task, and change roles.
- [`LibraryWriteLane`](../../app/runtime/library/LibraryWriteLane.h) owns the live-runtime FIFO command lane, move-only value admission permits, task admission, revision validation, commit, publication settlement, Maintenance, and Closing.
- [`LibrarySnapshot`](../../app/include/ao/rt/library/LibrarySnapshot.h) and [`LibraryCommands`](../../app/include/ao/rt/library/LibraryCommands.h) define scoped reads and owning asynchronous mutation commands.
- [`LibraryJobs`](../../app/include/ao/rt/library/LibraryJobs.h) defines asynchronous library operations, best-effort progress, and status-free progress finalization.
- [`LibraryImportPlan`](../../app/include/ao/rt/library/LibraryImportPlan.h) is the one-shot preview-bound import capability.
- [`LibraryChanges`](../../app/include/ao/rt/library/LibraryChanges.h) publishes revisioned committed changes.
- [`TrackSourceCache`](../../app/include/ao/rt/source/TrackSourceCache.h) owns reusable sources and their dependency graph.
- Source-private [`ListOrderSource`](../../app/runtime/source/ListOrderSource.h) derives effective saved-List order from raw rank plus current predicate membership.
- [`TrackListProjection`](../../app/include/ao/rt/projection/TrackListProjection.h) is the concrete ordered-list projection boundary.
- [`CoreRuntime.cpp`](../../app/runtime/CoreRuntime.cpp) is the ownership and lifetime composition root for the subsystem.

## Test map

- [`MusicLibraryTest.cpp`](../../test/unit/library/MusicLibraryTest.cpp) protects closed-world admission, version-before-flags precedence, one-open-per-named-DBI behavior, complete Store and cross-Store validation, accepted opaque states, linear open work, store composition, same-library and cross-process writer exclusion, and transaction-anchored lease lifetime.
- [`MetadataStoreTest.cpp`](../../test/unit/library/MetadataStoreTest.cpp) protects logical metadata snapshots and durable-snapshot candidate revision publication across commit failure and a child-process commit.
- [`WriteTransactionTest.cpp`](../../test/unit/library/WriteTransactionTest.cpp) protects the callback-scoped mutation capability, root operation success, recoverable mutation/read failure rollback, unexpected-exception containment, terminal state, and immediate writer-gate reuse.
- [`TrackWriterTest.cpp`](../../test/unit/library/TrackWriterTest.cpp) protects the public/physical capability boundary and coherent Track, manifest, dictionary, Resource, update, relink, delete, and clear behavior.
- [`ListWriterTest.cpp`](../../test/unit/library/ListWriterTest.cpp) protects live parent validation, leaf-delete conflict, subtree ordering, and coherent clear behavior.
- [`TrackStoreTest.cpp`](../../test/unit/library/TrackStoreTest.cpp) and [`TrackStoreRawLayoutTest.cpp`](../../test/unit/library/TrackStoreRawLayoutTest.cpp) protect batch order, missing-row behavior, the complete Reader range, physical-side projections, coordinated hot/cold traversal, and prepared record writes.
- [`TrackStoreIntegrityTest.cpp`](../../test/unit/library/TrackStoreIntegrityTest.cpp) protects reserved-id rejection and fail-closed rejection of non-canonical persisted records.
- [`DatabaseWriterTest.cpp`](../../test/unit/lmdb/DatabaseWriterTest.cpp) protects the copied-data-only public LMDB writer boundary and the source-private reservation encoder contract.
- [`ListBuilderTest.cpp`](../../test/unit/library/ListBuilderTest.cpp), [`ListStoreTest.cpp`](../../test/unit/library/ListStoreTest.cpp), [`FileManifestBuilderTest.cpp`](../../test/unit/library/FileManifestBuilderTest.cpp), and [`FileManifestStoreTest.cpp`](../../test/unit/library/FileManifestStoreTest.cpp) protect prepared snapshots, binding-independent manifest validation, prepared-only writer surfaces, local record validation, and post-open iterator fail-fast behavior.
- [`LibraryProbeTest.cpp`](../../test/unit/library/LibraryProbeTest.cpp) protects prepared-write preconditions, post-open Store and cross-Store trust, revision exhaustion, LMDB lifetime contracts, and bounded normal child-process observations.
- [`RuntimeFatalProbeTest.cpp`](../../test/unit/runtime/library/RuntimeFatalProbeTest.cpp) protects runtime consumers that enforce admitted cross-Store references before producing external output.
- [`DictionaryStoreTest.cpp`](../../test/unit/library/DictionaryStoreTest.cpp) protects NFC identity, malformed-input rejection, overlay rollback, terminal commit-failure recovery, writer lifetime across transaction completion, stable borrowed views, bounded-cache behavior, batch binding, and all-or-none concurrent publication.
- [`PlanEvaluatorDictionaryTest.cpp`](../../test/unit/query/PlanEvaluatorDictionaryTest.cpp) protects bound dictionary predicates and explicit unresolved-symbol semantics.
- [`LibrarySnapshotTest.cpp`](../../test/unit/runtime/library/LibrarySnapshotTest.cpp) and [`LibraryCommandsTest.cpp`](../../test/unit/runtime/library/LibraryCommandsTest.cpp) protect nonmovable facade identity and runtime access roles.
- [`LibraryChangesTest.cpp`](../../test/unit/runtime/library/LibraryChangesTest.cpp) protects revision ordering, callback publication, signal-before-await settlement, Mutation move chains and exactly-once admission release, Maintenance workflow ordering, queued-delivery retirement, and Closing retirement.
- [`LibraryAuthoringTest.cpp`](../../test/unit/runtime/library/LibraryAuthoringTest.cpp) protects availability, binding validation, non-terminal contention, all-or-none authoring, independent post-grant cancellation, transaction-construction unwind, pre-transaction Closing cancellation, and reentrant publication closure.
- [`LibraryJobsTest.cpp`](../../test/unit/runtime/library/LibraryJobsTest.cpp) protects worker/callback task boundaries.
- [`TrackSourceCacheTest.cpp`](../../test/unit/runtime/source/TrackSourceCacheTest.cpp) protects source lifetime, reuse, and refresh composition.
- [`ListOrderSourceTest.cpp`](../../test/unit/runtime/source/ListOrderSourceTest.cpp) and [`ListOrderSourceObserverTest.cpp`](../../test/unit/runtime/source/ListOrderSourceObserverTest.cpp) protect rank derivation, hidden ranks, parent changes, and delta translation.
- [`TrackListProjectionLifecycleTest.cpp`](../../test/unit/runtime/projection/TrackListProjectionLifecycleTest.cpp) and [`TrackListProjectionDeltaContractTest.cpp`](../../test/unit/runtime/projection/TrackListProjectionDeltaContractTest.cpp) protect the source-to-projection boundary.
- [`TrackListProjectionGroupingTest.cpp`](../../test/unit/runtime/projection/TrackListProjectionGroupingTest.cpp) protects normalized grouping keys and raw presentation labels.

## Related documents

- [Decision 0015: sequence live-runtime library writes](../decision/0015-sequence-live-runtime-library-writes.md)
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
