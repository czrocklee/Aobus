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
- **Submission** is the private coordinator handoff of one committed changeset and its terminal callback.
- **Publication** is ordered delivery of one revision on the callback executor, in two phases.
- **Replica** is the single bound consumer that keeps the derived state the rest of the runtime reads through. At most one is bound at a time.
- **Notification** is phase-two observer delivery after replica application.
- **Publication terminal** is `Published` after both delivery phases, or `RetiredByClosing` when coordinated Closing retires an admitted delivery that has not begun.
- **Revision settlement** is the sequencer acknowledgement after publication reaches its terminal and, for an available runtime, the resulting revision availability observers have returned.

## Invariants

- The revision is bumped inside the same write transaction as its content mutation.
- An aborted or preview transaction does not advance the revision.
- A producer submits a changeset only after the corresponding transaction commits.
- Only `LibraryMutationService` can submit committed content changes; ordinary consumers receive a const observation surface.
- A live operation supplies its complete zero-revision changeset inside `Changed`; `Mutation::executeAsync()` stamps and publishes that exact moved value, so a caller cannot choose a different post-operation payload.
- The coordinator submits exactly the expected successor revision and retains its one command-lane turn until publication settlement, so a second live commit cannot begin.
- The coordinator constructs every actor-owned state required for settlement and Closing retirement before native commit; after commit it moves only already-prepared handoff state into the non-throwing publication owner boundary.
- The bus rejects rather than buffers any revision other than that exact successor.
- A revision is announced to observers only after the bound replica applies it.
- A callback observes the complete committed library state described by its changeset, including every dictionary mapping referenced by changed records.
- Authoring becomes available at revision `R` only after publication delivery for `R`; an ordinary command does not complete until the resulting availability observers have also returned.
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
The coordinator's single command lane makes one in-flight submission the normal production topology.

Handlers run on the configured callback executor.
The coordinator holds no state mutex while invoking the replica, phase-two observers, or resulting availability observers.
The active command phase preserves serialization across those external callbacks.

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
The terminal callback advances coordinator availability only after both phases return. The publication
owner passes owned copies of its immutable library identity and pinned replica name across that callback boundary;
the coordinator does not reconstruct either label. The callback is an ordinary throwing callable contained by the
publication owner's non-throwing delivery boundary, so an escape is fatal with publication context.
When authoring mode is `Available`, the coordinator updates the available revision and synchronously delivers the resulting availability notification before signalling the command's publication event.
When mode is `Maintenance`, it updates the committed publication state but emits no `Available`; the Maintenance workflow later exits through a distinct control command and owns that final availability delivery.
An `Available(R)` handler may bind targets at `R` because projections are already current, but a mutation attempted reentrantly from that handler is rejected until the notification returns.

A callback-thread attempt to mutate through the same coordinator while publication is active is rejected as reentrant.
Another command waits by coroutine suspension for its lane turn rather than blocking a callback or worker thread.

Before native commit, the coordinator prepares the publication terminal callback, one-shot event, result storage, and other correctness-critical state needed to terminalize the command.
After native commit it records `SubmittingPublication` before calling the non-throwing `publishFromCoordinator()` boundary.
The sequencer always submits from a worker; `Executor::dispatch()` admits `P(R)` to the serialized callback owner and returns, but that owner may nevertheless run the complete delivery concurrently before the worker's submission call returns.
The one-shot event therefore stores a terminal that arrives before await registration as well as a waiter registered first.
Only after submission returns does the command enter `AwaitingPublication` and await that event.
Event completion is posted to the awaiter's associated worker executor, so the sequencer cannot continue into the next transaction on the callback executor.
After the event resolves, the command terminalizes its actor-owned result, releases the lane, and permits the caller continuation to resume separately on its own executor.

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
It first seals command/task admission and retires queued command requests outside the state lock.
An active pre-transaction or transaction command receives its Closing stop request and reaches abort, commit, or another defined phase before the close loop advances.
Closing never seals `LibraryChanges` while an active command can still enter publication: `SubmittingPublication` must return first.
At `AwaitingPublication`, Closing unconditionally seals the bus and retires a not-yet-claimed pending publication; an already-running delivery completes normally as `Published`.
The terminal callback is moved out of the `LibraryChanges` mutex before `RetiredByClosing` is delivered, so coroutine resumption and coordinator calls never occur under that mutex.
At idle, Closing seals the bus even when no publication is pending, then declares the lane closed only after its command queue and active publication/control events are empty.
Only then may callback resumption close and workers stop.
An observer must not invoke Closing on the same synchronous publication stack and must defer teardown to a later callback-executor turn.
Queued delivery retains only weak bus state, and coordinator acknowledgements retain only weak coordinator lifetime state, so discarded callbacks never re-enter a retired owner.
`RetiredByClosing` is an internal durable-command terminal, not a claim that its transaction rolled back; a surviving public waiter uses the existing `OperationCancelled` control-flow channel.

## Persistence and versioning

Revision zero represents no committed published mutation in a fresh library.
Revision storage is defined by the [library database reference](../../../reference/library/storage/database.md).
Changesets are in-process values and have no persisted or compatibility format.

## Implementation map

- [`LibraryChanges.h`](../../../../app/include/ao/rt/library/LibraryChanges.h) defines changesets and subscriptions.
- [`LibraryChanges.cpp`](../../../../app/runtime/library/LibraryChanges.cpp) owns strict successor admission, one pending publication, executor delivery, the pinned replica, and phase-specific post-commit fatal context.
- [`Signal.h`](../../../../include/ao/async/Signal.h) carries the owning non-throwing notification boundary phase two uses.
- [`TrackSourceCache.cpp`](../../../../app/runtime/source/TrackSourceCache.cpp) applies revisions to the runtime's derived source state.
- [`LibraryMutationService.h`](../../../../app/runtime/library/LibraryMutationService.h) owns sequencer submission, one-shot settlement events, availability acknowledgement, and Closing state.
- [`MusicLibrary`](../../../../include/ao/library/MusicLibrary.h) exposes admitted snapshot and candidate revision values without a public physical metadata Store.
- [`WriteTransaction`](../../../../include/ao/library/WriteTransaction.h) derives the candidate from its durable writer snapshot, persists it at commit, and completes dictionary and metadata publication before the producer can submit a changeset.

## Test map

- [`LibraryChangesTest.cpp`](../../../../test/unit/runtime/library/LibraryChangesTest.cpp) proves in-band revisions, abort behavior, single-replica binding, unbound publication, completion-before-await ordering, Maintenance revision/workflow settlement, sequencer contention, and queued-callback retirement.
- [`TrackSourceCacheTest.cpp`](../../../../test/unit/runtime/source/TrackSourceCacheTest.cpp) proves source-state application.
- [`LibraryAuthoringTest.cpp`](../../../../test/unit/runtime/library/LibraryAuthoringTest.cpp) proves replica-before-notification-before-availability ordering and publication/availability reentrancy closure.
- Writer, scan, and transfer tests under [`test/unit/runtime/library/`](../../../../test/unit/runtime/library/) prove changeset contents and post-commit visibility.
- [`SourcePipelineOracleTest.cpp`](../../../../test/unit/runtime/source/SourcePipelineOracleTest.cpp) proves downstream state matches recomputation across mutation sequences.
- The library-publication scenarios in [`RuntimeFatalProbeScenario.cpp`](../../../../test/fatal/RuntimeFatalProbeScenario.cpp) and their expectations in [`RuntimeFatalProbeProtocol.cpp`](../../../../test/fatal/RuntimeFatalProbeProtocol.cpp) prove admission, replica, observer, and completion escapes abort with actual library path, revision, replica, phase, exception, and owning call-site context; the completion-ack policy probe protects the same identity and revision fields for an impossible acknowledgement.

## Related documents

- [Decision 0015: sequence live-runtime library writes](../../../decision/0015-sequence-live-runtime-library-writes.md)
- [Library architecture](../../../architecture/library.md)
- [Library access and mutation](mutation.md)
- [Track sources](../source/track-source.md)
- [Track-list projection](../projection/track-list.md)
