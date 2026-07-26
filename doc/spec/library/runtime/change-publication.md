---
id: library.change-publication
type: spec
status: current
domain: library
summary: Defines strict revision admission, two-phase changeset publication through one noexcept replica, and library task notification delivery.
---
# Library change publication

## Scope

This specification defines the committed revision stream exposed by `LibraryChanges` and its separate library-task progress channels.
It owns revision allocation, changeset categories, ordering, replica application, callback affinity, reentrancy visibility, and publication lifetime.

Mutation semantics belong to [library access and mutation](mutation.md).
Source consumption of changesets belongs to [track sources](../source/track-source.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
`LibraryChanges` is public under `app/include/ao/rt/library/`, implemented in `app/runtime/library/`, and bridges committed core-library revisions into runtime source consumers without depending on source or projection implementations.

## Terminology

- **Revision** is the unsigned 64-bit committed library sequence stored in the metadata database.
- **Expected revision** is the only revision the bus accepts after the last acknowledged publication.
- **Submission** is the private coordinator handoff of one committed changeset and its completion callback.
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
- Task progress and completion are operational notifications and do not consume library revisions.
- Releasing a subscription prevents later delivery to that subscriber.

## Changeset surface

One `LibraryChangeSet` may describe:

- complete library reset;
- inserted, deleted, and metadata-mutated track ids;
- upserted and deleted list ids;
- exact manual-list insert, remove, move, or reset operations.

Manual remove and move operations carry descending stored-coordinate removals.
Insert coordinates are measured in the sequence after preceding operations in the same change.
One committed transaction produces at most one changeset.

## Ordering and delivery

The default changes bus publishes synchronously for focused tests.
Production construction receives a callback executor and the last already-published revision.

The first submission to the default test bus establishes its revision baseline.
The production bus expects exactly one greater than the supplied last-published revision.
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
The completion callback advances coordinator availability only after both phases return.
The coordinator keeps writer admission closed while it delivers the resulting availability notification.
An `Available(R)` handler may bind targets at `R` because projections are already current, but a mutation attempted reentrantly from that handler is rejected until the notification returns.

A callback-thread attempt to mutate through the same coordinator while publication is active is rejected as reentrant.
A foreign worker waits for publication completion before acquiring writer ownership.

## Task notifications

`LibraryTaskService` reports best-effort progress as a fraction and message and reports completion with an operation count.
These notifications share the callback delivery boundary but are not persistence authority and do not imply track/list mutation.

Each committed identity-backfill batch advances the library revision and may publish a changeset with no track/list categories because it changes only manifest identity.
Scan and YAML operations publish content changes through normal revisioned changesets after commit.

## Failure and lifetime

A failed or aborted pre-commit mutation submits nothing.
After durable commit, revision-admission or executor enqueue failure completes the coordinator with that exception, moves authoring to terminal `Faulted`, and rejects later live-runtime mutations.
Replica and notification handlers are `noexcept`; an exception terminates instead of being translated into recovery, refusal, or branch-local availability.
The committed revision is not reported as an ordinary failed transaction and is not rolled back or followed by an inferred reset.
The direct caller or callback-executor boundary surfaces the failure after the coordinator has entered `Faulted`; the notification framework does not reinterpret it.
Reopening the runtime rebuilds consumers from durable storage.

The bus outlives its subscriptions and all producers.
Runtime teardown stops and joins task producers before destroying `LibraryChanges`.
Queued delivery retains only weak bus state, and coordinator completions retain
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
