---
id: library.change-publication
type: spec
status: current
domain: library
summary: Defines revision allocation, ordered changeset publication, and library task notification delivery.
---
# Library change publication

## Scope

This specification defines the committed revision stream exposed by `LibraryChanges` and its separate library-task progress channels.
It owns revision allocation, changeset categories, ordering, callback affinity, reentrancy visibility, and publication lifetime.

Mutation semantics belong to [library access and mutation](mutation.md).
Source consumption of changesets belongs to [track sources](../source/track-source.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
`LibraryChanges` is public under `app/include/ao/rt/library/`, implemented in `app/runtime/library/`, and bridges committed core-library revisions into runtime source consumers without depending on source or projection implementations.

## Terminology

- **Revision** is the unsigned 64-bit committed library sequence stored in the metadata database.
- **Submission** is the private coordinator handoff of one committed changeset and its completion callback.
- **Publication** is ordered observer delivery on the callback executor.
- **Holdback** is the set of submitted changesets waiting for preceding revisions.
- **Publication completion** is the coordinator acknowledgement after every still-connected synchronous observer has run for one revision.

## Invariants

- The revision is bumped inside the same write transaction as its content mutation.
- An aborted or preview transaction does not advance the revision.
- A producer submits a changeset only after the corresponding transaction commits.
- Only `LibraryMutationService` can submit committed content changes; ordinary consumers receive a const observation surface.
- Observers receive committed changesets in strictly increasing contiguous revision order even when producers submit out of order.
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

Submitting revision `N` before an expected lower revision retains `N` in holdback.
When the missing revision arrives, the bus delivers it and then drains every now-contiguous held revision.
No later revision overtakes an earlier one.

Handlers run on the configured callback executor.
Every still-connected handler runs even when an earlier handler throws; the first observer failure is retained until delivery and coordinator completion finish.
The completion callback advances coordinator availability only after those handlers return.
The coordinator keeps writer admission closed while it delivers the resulting availability notification.
An `Available(R)` handler may bind targets at `R` because projections are already current, but a mutation attempted reentrantly from that handler is rejected until the notification returns.

A callback-thread attempt to mutate through the same coordinator while publication is active is rejected as reentrant.
A foreign worker waits for publication completion before acquiring writer ownership.
Nested revisions from independent test producers may enter holdback, but current-revision delivery completes before any later revision is observed.

## Task notifications

`LibraryTaskService` reports best-effort progress as a fraction and message and reports completion with an operation count.
These notifications share the callback delivery boundary but are not persistence authority and do not imply track/list mutation.

Each committed identity-backfill batch advances the library revision and may publish a changeset with no track/list categories because it changes only manifest identity.
Scan and YAML operations publish content changes through normal revisioned changesets after commit.

## Failure and lifetime

A failed or aborted pre-commit mutation submits nothing.
After durable commit, executor enqueue failure or any observer failure completes the coordinator with that exception, moves authoring to terminal `Faulted`, and rejects later live-runtime mutations.
The committed revision is not reported as an ordinary failed transaction and is not rolled back or followed by an inferred reset.
An executor that contains callback exceptions may log rather than propagate the observer exception to the synchronous caller; the coordinator state still becomes `Faulted`, and authoring sessions reconcile that state before accepting a returned binding.
Reopening the runtime rebuilds consumers from durable storage.

The bus outlives its subscriptions and all producers.
Runtime teardown stops and joins task producers before destroying `LibraryChanges`.

## Persistence and versioning

Revision zero represents no committed published mutation in a fresh library.
Revision storage is defined by the [library database reference](../../../reference/library/storage/database.md).
Changesets are in-process values and have no persisted or compatibility format.

## Implementation map

- [`LibraryChanges.h`](../../../../app/include/ao/rt/library/LibraryChanges.h) defines changesets and subscriptions.
- [`LibraryChanges.cpp`](../../../../app/runtime/library/LibraryChanges.cpp) owns holdback and executor delivery.
- [`LibraryMutationService.h`](../../../../app/runtime/library/LibraryMutationService.h) owns submission and publication-completion state.
- [`MetadataStore`](../../../../include/ao/library/MetadataStore.h) owns in-transaction revision reads and bumps.
- [`WriteTransaction`](../../../../include/ao/library/WriteTransaction.h) completes dictionary-index publication before the producer can submit a changeset.

## Test map

- [`LibraryChangesTest.cpp`](../../../../test/unit/runtime/library/LibraryChangesTest.cpp) proves in-band revisions, abort behavior, multi-worker holdback, and publication order.
- [`LibraryAuthoringTest.cpp`](../../../../test/unit/runtime/library/LibraryAuthoringTest.cpp) proves projection-before-availability ordering, reentrancy closure, and enqueue/observer fault behavior.
- Writer, scan, and transfer tests under [`test/unit/runtime/library/`](../../../../test/unit/runtime/library/) prove changeset contents and post-commit visibility.
- [`SourcePipelineOracleTest.cpp`](../../../../test/unit/runtime/source/SourcePipelineOracleTest.cpp) proves downstream state matches recomputation across mutation sequences.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Library access and mutation](mutation.md)
- [Track sources](../source/track-source.md)
- [Track-list projection](../projection/track-list.md)
