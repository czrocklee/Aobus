---
id: rfc.0003.library-mutation-pipeline
type: rfc
status: draft
domain: library
summary: Proposes one runtime mutation gateway that couples write scheduling, commit revision, and change publication.
depends-on: none
---
# RFC 0003: Unified library mutation pipeline

## Problem

The runtime library facade currently has two partially independent mutation paths.
`LibraryTaskService` serializes import, scan application, and identity write-back with a private mutex, while synchronous `LibraryWriter` commands open LMDB write transactions without participating in that coordination.
LMDB still enforces one physical writer, but application-level scheduling, callback-thread blocking, cancellation priority, and operation ordering do not have one owner.

Every public `MusicLibrary::writeTransaction()` call increments the library revision inside the transaction.
After commit, each runtime caller constructs and publishes its own `LibraryChangeSet`.
A committed low-level transaction that omits publication creates a revision gap, and production `LibraryChanges` retains later revisions while waiting for the missing event.
The low-level `CoreRuntime::musicLibrary()` escape hatch makes this invariant possible to violate outside the normal writer.

The current boundaries are described by the [library architecture](../architecture/library.md), [mutation specification](../spec/library/runtime/mutation.md), and [change-publication specification](../spec/library/runtime/change-publication.md).

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Give every application mutation one scheduling and commit owner.
- Make revision allocation, durable commit, and semantic change construction one protocol that normal callers cannot partially perform.
- Prevent synchronous frontend commands from blocking behind unbounded worker transactions.
- Preserve ordered callback-executor publication when mutations prepare or finish out of order.
- Keep read snapshots independent from write scheduling.
- Retain a narrow core administration surface for migrations, fixtures, and offline tools without exposing it as a normal runtime extension point.

## Non-goals

- Replace LMDB transaction semantics.
- Turn all synchronous commands into public asynchronous APIs immediately.
- Define scan batching, YAML schema, or database migration policy.
- Guarantee that in-memory observers receive an event after process termination.
- Let frontend code construct physical store writers.

## Proposed design

### Runtime mutation coordinator

`ao::rt::Library` owns one `LibraryMutationCoordinator` beside its reader, writer, task, and changes roles.
The coordinator is the only normal runtime component allowed to begin a committing `MusicLibrary` write transaction.

Mutations have two conceptual phases:

```text
prepare without writer ownership
  -> enqueue commit intent
  -> begin write transaction
  -> revalidate affected state
  -> apply writes and build semantic change set
  -> obtain revision and commit
  -> publish committed change set on callback executor
```

Small `LibraryWriter` commands may perform preparation and commit together when their work is bounded and does not perform slow filesystem I/O.
Long tasks prepare on worker executors and hold coordinator writer ownership only for revalidation and bounded database writes.

### Commit envelope

The coordinator creates an internal `LibraryMutation` envelope containing the write transaction, mutation kind, affected identities, and a semantic change accumulator.
Callers add typed inserted, deleted, mutated, relinked, list, or reset facts through the envelope rather than constructing an arbitrary post-commit changeset.

The only successful terminal operation is `commitAndPublish`:

1. Validate that the semantic change is consistent with the writes and reset mode.
2. Read the revision allocated inside the transaction.
3. Finalize a nonzero `LibraryChangeSet` before commit.
4. Commit the transaction.
5. Enqueue the finalized change for ordered callback delivery through a no-loss runtime queue.

Abort and preview never publish and never consume a durable revision.
A committed change that cannot be enqueued triggers a coordinator fault state and a callback-side library reset from current storage rather than allowing an unbounded revision gap.

### Scheduling and affinity

The coordinator owns a FIFO commit queue with explicit operation categories for interactive commands, background batches, and exclusive maintenance.
It does not hold an application mutex during file parsing, hashing, YAML parsing, or callback execution.
Background operations yield between bounded commits so interactive mutations can make progress.

Callback-thread commands that cannot begin immediately are adapted to task or command-completion results rather than blocking the callback executor on LMDB writer acquisition.
The public facade may retain synchronous methods only where the coordinator can prove their commit path is bounded and immediately available.

### Publication recovery

`LibraryChanges` retains revision ordering for producers that finish preparation out of order.
Its holdback is bounded and observable.
A gap that cannot be produced by the coordinator is treated as an invariant failure and recovers by publishing one library reset at the latest committed revision after rebuilding dependent caches from storage.

Duplicate and stale revisions remain errors.
Tests and production use the same initial-revision and callback-executor ordering mode.

### Low-level access

`MusicLibrary` continues to expose read transactions and physical stores to core library code.
Committing write-transaction construction becomes private to a core mutation token or another capability supplied only to the runtime coordinator, migration machinery, and focused test fixtures.

`CoreRuntime::musicLibrary()` remains available for read-only inspection during migration of existing callers.
Administration that performs writes uses an explicit offline or exclusive mutation capability and cannot share a live runtime change bus accidentally.

## Alternatives

### Share the existing mutex with `LibraryWriter`

This serializes more callers but still couples neither revision nor publication to commit and may block the callback thread for an unbounded task transaction.
It is an interim containment measure rather than a complete application boundary.

### Rely only on LMDB's single-writer lock

LMDB protects physical consistency but does not own application affinity, semantic changes, fairness, or observer recovery.

### Publish a generic reset after every commit

This eliminates semantic revision gaps but discards incremental source and projection behavior and increases callback work for ordinary edits.

### Infer changes by diffing database snapshots

Snapshot diffing can recover from an exceptional gap but is too expensive and imprecise to replace typed mutation intent on every command.

## Compatibility and migration

The physical database layout and revision record do not need to change.
Existing `LibraryWriter`, `LibraryTaskService`, importer, scanner, and indexer implementations migrate one mutation family at a time behind the unchanged `ao::rt::Library` facade where practical.

During migration, a guard records whether a live runtime commit bypasses the coordinator and fails focused tests.
Direct-write CLI administration either adopts the coordinator or opens the database in an explicitly offline mode with no live observers.
Test fixtures receive a dedicated low-level mutation helper instead of treating the production write escape hatch as normal API.

## Validation

- Tests prove every successful runtime commit publishes exactly one matching nonzero revision or one documented coalesced envelope.
- Production-mode change-bus tests cover missing, duplicate, stale, and out-of-order revisions with a real callback executor.
- Failure injection covers apply failure, commit failure, enqueue failure, observer exception, cancellation before commit, and shutdown after commit.
- Concurrency tests run interactive Writer commands against scan, import, and identity batches and assert bounded callback-executor latency.
- Tests prove slow preparation never owns the mutation lock or an LMDB write transaction.
- Source and projection oracle tests prove semantic deltas and reset recovery converge to fresh storage snapshots.
- Boundary checks reject new frontend or runtime code that begins a raw committing transaction outside approved implementation areas.
- ThreadSanitizer and concurrency suites cover coordinator lifetime, queue shutdown, and callback publication.

## Open questions

- Which existing `LibraryWriter` commands can remain synchronously callable without violating the callback latency budget?
- Should the coordinator use strict FIFO scheduling or a bounded interactive-priority policy?
- Is a committed in-memory publication queue sufficient, or should pending semantic changes have a small durable journal?
- Which exceptional failures recover with a library reset and which should stop the runtime entirely?
- Can one changeset describe a bounded batch from multiple compatible command intents, or must every command retain a distinct revision?

## Promotion plan

- Update [library architecture](../architecture/library.md) with coordinator ownership and the reduced low-level escape hatch.
- Update [runtime execution architecture](../architecture/runtime-execution.md) with commit scheduling and callback affinity.
- Update the [mutation specification](../spec/library/runtime/mutation.md), [task-execution specification](../spec/library/runtime/task-execution.md), and [change-publication specification](../spec/library/runtime/change-publication.md).
- Update the [library database reference](../reference/library/storage/database.md) only if revision storage or an optional publication journal changes.
- Add a decision record for scheduling policy or durable publication if the rejected alternatives remain important.
- Update development boundary checks and concurrency validation guidance.
