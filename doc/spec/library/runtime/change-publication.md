---
id: library.change-publication
type: spec
status: current
domain: library
summary: Defines strict revision admission and two-phase changeset publication through one mandatory replica.
---
# Library change publication

## Scope

This specification defines the committed revision stream exposed by `LibraryChanges`.
It owns revision allocation, changeset categories, ordering, replica application, callback affinity, reentrancy visibility, and publication lifetime.

Mutation semantics belong to [library access and mutation](mutation.md).
Source consumption of changesets belongs to [track sources](../source/track-source.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
`LibraryChanges` is public under `app/include/ao/rt/library/`, implemented in `app/runtime/library/`, and bridges committed core-library revisions into runtime source consumers without depending on source or projection implementations.

## Terminology

- **Revision** is the unsigned 64-bit committed library sequence stored in the metadata database.
- **Expected revision** is the only revision the bus accepts after the last acknowledged publication.
- **Submission** is the private coordinator handoff of one committed changeset and its completion acknowledgement.
- **Publication** is ordered delivery of one revision on the callback executor, in two phases.
- **Replica** is the single bound consumer that keeps the derived state the rest of the runtime reads through. At most one is bound at a time.
- **Notification** is phase-two observer delivery after replica application.
- **Publication completion** is the coordinator acknowledgement after both phases have run for one revision.

## Invariants

- The revision is bumped inside the same write transaction as its content mutation.
- An aborted or preview transaction does not advance the revision.
- A producer submits a changeset only after the corresponding transaction commits.
- Only `LibraryMutationService` can submit committed content changes; ordinary consumers receive a const observation surface.
- The coordinator submits exactly the expected successor revision and keeps a second commit closed while publication is active.
- The bus rejects rather than buffers any revision other than that exact successor.
- A revision is announced to observers only after the bound replica applies it.
- A callback observes the complete committed library state described by its changeset, including every dictionary mapping referenced by changed records.
- Authoring becomes available at revision `R` only after publication completion for `R`.
- Releasing a subscription prevents later delivery to that subscriber.
- `LibraryChanges` is the only authoritative path from a committed library fact to runtime replicas, sources, projections, and frontend cache invalidation.
- Task-progress finalization and workflow callbacks never infer or trigger a second data refresh.

## Changeset surface

One `LibraryChangeSet` may describe:

- complete library reset;
- inserted, deleted, and metadata-mutated track ids;
- upserted and deleted list ids;
- a canonical regular remove/insert/update edit script or complete reset for each affected saved List raw order.

Raw-order removals use descending stored-coordinate ranges.
A saved-order move is represented by those removals followed by one insertion in the sequence after all preceding edits.
Raw-order coordinates may include hidden ranks and are never projection row coordinates.
`ListOrderSource` is the only consumer that translates a raw-order change into an effective source delta.
One semantic Remove-from-Playlist command may carry `tracksMutated`, `listsUpserted`, and `listOrderChanges` together at one revision.
One committed transaction produces at most one changeset.
An identity-only manifest batch may publish an otherwise empty changeset so revision and publication ordering advance without claiming a Track or List mutation.

## Ordering and delivery

Construction always receives a callback executor, the last already-published revision, and one immutable diagnostic identity for the physical library.
Production uses the actual database path; tests that need inline delivery supply an inline executor, an explicit persisted baseline, and an explicit test identity.
The bus expects exactly one greater than the supplied last-published revision.
A submission with any other revision, or a second submission while that revision is being delivered, is rejected immediately rather than retained for later delivery.
The coordinator's publication barrier makes the single in-flight submission the normal production topology.

Handlers run on the configured callback executor.
The coordinator holds no writer or state mutex while invoking the replica, phase-two observers, or the resulting availability observers.
Logical publication, submission-admission, and availability-notification gates preserve serialization across those external callbacks.

Publication delivers one revision in two phases.
Phase one hands the changeset to the bound replica as an ordinary callable inside the publication owner's
non-throwing delivery boundary; an escaping exception is diagnosed and aborted rather than converted into a
publication result.
Phase two announces the applied revision to observers, so reaching an observer states that the library is readable at that revision.
A revision with no bound replica proceeds directly to notification.
Binding a new replica during active publication is rejected. Unbinding does
not interrupt a replica already pinned for that publication, but no replacement
may become current until publication completes.
The expected revision advances only after phase two returns.
The completion acknowledgement advances coordinator availability only after both phases return. The publication
owner passes owned copies of its immutable library identity and pinned replica name across that callback boundary;
the coordinator does not reconstruct either label. The callback is an ordinary throwing callable contained by the
publication owner's non-throwing delivery boundary, so an escape is fatal with publication context.
The coordinator keeps writer admission closed while it delivers the resulting availability notification.
An `Available(R)` handler may bind targets at `R` because projections are already current, but a mutation attempted reentrantly from that handler is rejected until the notification returns.

A callback-thread attempt to mutate through the same coordinator while publication is active is rejected as reentrant.
A foreign worker waits for publication completion before acquiring writer ownership.

The coordinator holds physical writer ownership through native commit, establishes publication and submission-admission gates, and then releases the writer mutex before submitting the mandatory publication task `P(R)`.
The submission-admission gate blocks another writer or Closing until that submission call has either completed `P(R)` inline or returned after the callback executor accepted it.
The publication gate remains closed until `P(R)` completes or coordinated Closing retires a not-yet-running submission.
An owner-thread submission executes `P(R)` inline before commit returns.
For a foreign submission, `P(R)` is accepted before the same maintenance operation admits its non-cancellable callback-owner finalization `C`.
The callback executor may complete `P(R)` before the foreign submission call returns; writer admission remains closed until submission and completion have both rendezvoused in either order.
The production GTK, TUI, CLI, and WinUI executors derive from `QueuedExecutorBase`, so those two foreign admissions execute in FIFO order: `P(R)` completes before `C` can release maintenance or return the task result.
This is a causal guarantee for those admissions by one operation, not a global order across unrelated producers or owner-inline work.

## Failure and lifetime

A failed or aborted pre-commit mutation submits nothing and keeps its ordinary typed failure channel.
From native commit through completion of mandatory publication, a revision invariant, task-admission failure, or delivery failure in a live runtime terminates the process after best-effort logging.
Replica, notification, and publication-completion callables are ordinary callables contained by non-throwing owning
boundaries; they do not create a recovery branch, and an escape receives AO diagnostic context before abort.
The publication owner supplies the immutable library identity, committed revision, failure phase, and the pinned replica name or an explicit unbound marker.
The named phases distinguish executor admission, replica application, observer delivery, publication completion, and coordinator completion acknowledgement.
The committed revision is not reported as an ordinary failed transaction, rolled back, followed by an inferred reset, or exposed through a committed-but-unpublished outcome.
The next process open rebuilds consumers from durable storage.

Coordinated Closing is the sole retirement exception.
It waits for an in-flight submission call to leave its admission gate, takes physical writer ownership, seals new writer/task admission, retires any not-yet-running publication and maintenance-finalization callbacks, wakes publication waiters, and only then closes callback resumption and joins workers.
An observer must not invoke Closing on the same synchronous publication stack and must defer teardown to a later callback-executor turn.
An already-running publication completes under its owning non-throwing delivery boundary.
Queued delivery retains only weak bus state, and coordinator acknowledgements retain only weak coordinator lifetime state, so discarded callbacks never re-enter a retired owner.

## Persistence and versioning

Revision zero represents no committed published mutation in a fresh library.
Revision storage is defined by the [library database reference](../../../reference/library/storage/database.md).
Changesets are in-process values and have no persisted or compatibility format.

## Implementation map

- [`LibraryChanges.h`](../../../../app/include/ao/rt/library/LibraryChanges.h) defines changesets and subscriptions.
- [`LibraryChanges.cpp`](../../../../app/runtime/library/LibraryChanges.cpp) owns strict successor admission, one pending publication, executor delivery, the pinned replica, and phase-specific post-commit fatal context.
- [`Signal.h`](../../../../include/ao/async/Signal.h) carries the owning non-throwing notification boundary phase two uses.
- [`TrackSourceCache.cpp`](../../../../app/runtime/source/TrackSourceCache.cpp) applies revisions to the runtime's derived source state.
- [`LibraryMutationService.h`](../../../../app/runtime/library/LibraryMutationService.h) owns submission and publication-completion state.
- [`MusicLibrary`](../../../../include/ao/library/MusicLibrary.h) exposes admitted snapshot and candidate revision values without a public physical metadata Store.
- [`WriteTransaction`](../../../../include/ao/library/WriteTransaction.h) derives the candidate from its durable writer snapshot, persists it at commit, and completes dictionary and metadata publication before the producer can submit a changeset.

## Test map

- [`LibraryChangesTest.cpp`](../../../../test/unit/runtime/library/LibraryChangesTest.cpp) proves in-band revisions, abort behavior, single-replica binding, unbound publication, both foreign submission/completion orders, publication-before-finalization ordering, and queued-callback retirement.
- [`TrackSourceCacheTest.cpp`](../../../../test/unit/runtime/source/TrackSourceCacheTest.cpp) proves source-state application.
- [`LibraryAuthoringTest.cpp`](../../../../test/unit/runtime/library/LibraryAuthoringTest.cpp) proves replica-before-notification-before-availability ordering and publication/availability reentrancy closure.
- Writer, scan, and transfer tests under [`test/unit/runtime/library/`](../../../../test/unit/runtime/library/) prove changeset contents and post-commit visibility.
- [`SourcePipelineOracleTest.cpp`](../../../../test/unit/runtime/source/SourcePipelineOracleTest.cpp) proves downstream state matches recomputation across mutation sequences.
- The library-publication scenarios in [`RuntimeFatalProbeScenario.cpp`](../../../../test/fatal/RuntimeFatalProbeScenario.cpp) and their expectations in [`RuntimeFatalProbeProtocol.cpp`](../../../../test/fatal/RuntimeFatalProbeProtocol.cpp) prove admission, replica, observer, and completion escapes abort with actual library path, revision, replica, phase, exception, and owning call-site context; the completion-ack policy probe protects the same identity and revision fields for an impossible acknowledgement.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Library access and mutation](mutation.md)
- [Track sources](../source/track-source.md)
- [Track-list projection](../projection/track-list.md)
