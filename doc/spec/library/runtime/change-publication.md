---
id: library.change-publication
type: spec
status: current
domain: library
summary: Defines strict revision admission and two-phase changeset publication through one noexcept replica.
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
- **Submission** is the private coordinator handoff of one committed changeset and its `noexcept` completion acknowledgement.
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

## Ordering and delivery

Construction always receives a callback executor and the last already-published revision.
Tests that need inline delivery supply an inline executor and an explicit persisted baseline.
The bus expects exactly one greater than the supplied last-published revision.
A submission with any other revision, or a second submission while that revision is being delivered, is rejected immediately rather than retained for later delivery.
The coordinator's publication barrier makes the single in-flight submission the normal production topology.

Handlers run on the configured callback executor.

Publication delivers one revision in two phases.
Phase one hands the changeset to the bound replica through a `noexcept` callable; failure is fatal rather than converted into a publication result.
Phase two announces the applied revision to observers, so reaching an observer states that the library is readable at that revision.
A revision with no bound replica proceeds directly to notification.
Binding a new replica during active publication is rejected. Unbinding does
not interrupt a replica already pinned for that publication, but no replacement
may become current until publication completes.
The expected revision advances only after phase two returns.
The `noexcept` completion acknowledgement advances coordinator availability only after both phases return.
The coordinator keeps writer admission closed while it delivers the resulting availability notification.
An `Available(R)` handler may bind targets at `R` because projections are already current, but a mutation attempted reentrantly from that handler is rejected until the notification returns.

A callback-thread attempt to mutate through the same coordinator while publication is active is rejected as reentrant.
A foreign worker waits for publication completion before acquiring writer ownership.

## Failure and lifetime

A failed or aborted pre-commit mutation submits nothing.
After durable commit, revision-admission or executor enqueue failure propagates synchronously to the coordinator call site, moves authoring to terminal `Faulted`, and rejects later live-runtime mutations.
Replica and notification handlers are `noexcept`; an exception terminates instead of being translated into recovery, refusal, or branch-local availability.
The committed revision is not reported as an ordinary failed transaction and is not rolled back or followed by an inferred reset.
The direct caller or callback-executor boundary surfaces the failure after the coordinator has entered `Faulted`; the notification framework does not reinterpret it.
Reopening the runtime rebuilds consumers from durable storage.

The bus outlives its subscriptions and its coordinator producer.
Runtime teardown stops and joins library tasks before destroying the library facade and `LibraryChanges`.
Queued delivery retains only weak bus state, and coordinator acknowledgements retain
only weak coordinator lifetime state, so a retired owner is never re-entered.

## Persistence and versioning

Revision zero represents no committed published mutation in a fresh library.
Revision storage is defined by the [library database reference](../../../reference/library/storage/database.md).
Changesets are in-process values and have no persisted or compatibility format.

## Implementation map

- [`LibraryChanges.h`](../../../../app/include/ao/rt/library/LibraryChanges.h) defines changesets and subscriptions.
- [`LibraryChanges.cpp`](../../../../app/runtime/library/LibraryChanges.cpp) owns strict successor admission, one pending publication, and executor delivery.
- [`Signal.h`](../../../../include/ao/async/Signal.h) carries the noexcept notification contract phase two uses.
- [`TrackSourceCache.cpp`](../../../../app/runtime/source/TrackSourceCache.cpp) applies revisions to the runtime's derived source state.
- [`LibraryMutationService.h`](../../../../app/runtime/library/LibraryMutationService.h) owns submission and publication-completion state.
- [`MetadataStore`](../../../../include/ao/library/MetadataStore.h) owns in-transaction revision reads and bumps.
- [`WriteTransaction`](../../../../include/ao/library/WriteTransaction.h) completes dictionary-index publication before the producer can submit a changeset.

## Test map

- [`LibraryChangesTest.cpp`](../../../../test/unit/runtime/library/LibraryChangesTest.cpp) proves exact-successor admission, in-band revisions, abort behavior, single-replica binding, and unbound publication.
- [`TrackSourceCacheTest.cpp`](../../../../test/unit/runtime/source/TrackSourceCacheTest.cpp) proves source-state application.
- [`LibraryAuthoringTest.cpp`](../../../../test/unit/runtime/library/LibraryAuthoringTest.cpp) proves replica-before-notification-before-availability ordering, reentrancy closure, and enqueue fault behavior.
- Writer, scan, and transfer tests under [`test/unit/runtime/library/`](../../../../test/unit/runtime/library/) prove changeset contents and post-commit visibility.
- [`SourcePipelineOracleTest.cpp`](../../../../test/unit/runtime/source/SourcePipelineOracleTest.cpp) proves downstream state matches recomputation across mutation sequences.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Library access and mutation](mutation.md)
- [Track sources](../source/track-source.md)
- [Track-list projection](../projection/track-list.md)
