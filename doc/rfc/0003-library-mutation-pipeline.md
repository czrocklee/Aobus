---
id: rfc.0003.library-mutation-pipeline
type: rfc
status: implemented
domain: library
summary: Introduced one runtime mutation coordinator that couples write admission, durable commit, and ordered change publication.
depends-on: none
---
# RFC 0003: Unified library mutation pipeline

## Disposition

Implemented on 2026-07-16.

`WritableMusicLibrary` now holds the non-blocking per-database writer lease and is the only core capability that can begin a committing library transaction.
`MusicLibrary` and `CoreRuntime::musicLibrary()` remain read-oriented, while each transaction retains the lease anchor until commit, failure, or destruction.

One runtime-private `LibraryMutationService` owns that capability, interactive and exclusive-maintenance admission, contiguous revision validation, durable commit, and completion of ordered `LibraryChanges` publication.
Import, scan apply, and audio-identity backfill close interactive admission before slow worker preparation and take bounded write sessions only for apply/commit.
Failure before commit remains an ordinary typed operation error; enqueue or observer failure after commit moves the runtime to terminal `Faulted` state.

Public runtime headers, UIModel, GTK, and TUI cannot name committing storage authority.
CLI mutations use semantic runtime commands, while focused storage tests and explicitly offline scan/YAML operations construct the core capability directly.

The [library architecture](../architecture/library.md), [runtime execution architecture](../architecture/runtime-execution.md), [mutation specification](../spec/library/runtime/mutation.md), [task-execution specification](../spec/library/runtime/task-execution.md), and [change-publication specification](../spec/library/runtime/change-publication.md) now own current behavior and supersede the proposal wording below.

## Problem

The runtime library facade has two independent mutation paths.
`LibraryTaskService` serializes import, scan application, and identity write-back with a private mutex, while synchronous `LibraryWriter` commands open LMDB write transactions outside that coordination.
LMDB preserves physical single-writer consistency, but application admission, maintenance exclusion, revision ownership, and observer publication have no single owner.

Every public `MusicLibrary::writeTransaction()` allocates the next library revision inside the transaction.
After commit, each caller independently constructs and publishes a `LibraryChangeSet`.
A committed transaction that omits publication creates a revision gap, while a caller that reports an ordinary failure after commit tells the frontend that nothing changed even though durable state already did.

The low-level mutable `CoreRuntime::musicLibrary()` escape hatch and frontend migration seams make both failures expressible outside the intended runtime path.
The current boundaries are described by the [library architecture](../architecture/library.md), [mutation specification](../spec/library/runtime/mutation.md), and [change-publication specification](../spec/library/runtime/change-publication.md).

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Give every live-runtime mutation one admission, transaction, commit, and publication owner.
- Make a committing transaction unavailable to normal runtime, UIModel, and frontend callers.
- Serialize interactive writes with exclusive maintenance without holding writer ownership during slow preparation.
- Treat the persisted library revision as the single commit epoch.
- Publish every committed revision in order before authoring is advertised as available at that revision.
- Distinguish pre-commit failure from a committed mutation whose publication path faults.
- Prevent a second writable process from opening the same library concurrently.
- Retain an explicit offline core capability for storage tests and deliberately isolated administration.

## Non-goals

- Replace LMDB transaction semantics or add nested application transactions.
- Add a durable publication journal that survives process termination.
- Make long-lived read-only processes coherent with another process that commits new dictionary entries.
- Define metadata editor staleness or undo policy; [RFC 0023](0023-revision-bound-metadata-authoring.md) owns that policy.
- Add priority scheduling before a measured starvation problem exists.

## Proposed design

### Core writable capability

Core library code distinguishes the read-oriented `MusicLibrary` facade from an explicit writable capability.
The capability can begin a committing `WriteTransaction`; ordinary runtime public headers expose neither it nor the transaction.

`CoreRuntime` owns the capability privately and exposes only read-oriented library inspection.
Focused storage fixtures and explicitly offline administration may construct the capability directly, but live runtime services cannot manufacture independent copies.
The core capability knows nothing about runtime epochs, maintenance, UI sessions, or change publication.

### Writable-process lease

Opening a writable runtime acquires one non-blocking OS file lease associated with the database path and holds it for the runtime lifetime.
A second process may open the database read-only, but it cannot acquire a writable runtime until the first lease is released.

The lease prevents an external writer from advancing storage behind the runtime coordinator.
Cross-process refresh of the process-local dictionary index remains separate work; this RFC does not block all read-only processes merely to hide that existing limitation.

### Runtime mutation coordinator

`ao::rt::Library` owns one `LibraryMutationService` beside its reader, writer, task service, and changes roles.
The coordinator is the only live-runtime component allowed to consume the core writable capability.

Mutations use three admission classes:

| Class | Examples | Admission |
|---|---|---|
| Read-only | export, scan-plan construction, projections | Independent LMDB read snapshots |
| Interactive | metadata, tags, lists, track create/delete | Accepted only while authoring is available |
| Exclusive maintenance | scan apply, import, relink, identity backfill | Closes interactive admission for the whole operation |

Slow file walking, parsing, hashing, and media interpretation happen without a mutation session.
Maintenance may use several bounded commits, but each commit separately consumes one revision and publishes one matching change set.

### Commit and publication terminal states

One coordinator mutation follows this order:

```text
admit and acquire coordinator writer ownership
  -> begin core write transaction
  -> revalidate affected state
  -> apply writes and finalize semantic LibraryChangeSet
  -> read the transaction's nonzero library revision
  -> commit durable state
  -> release writer ownership
  -> publish that exact revision on the callback executor
  -> synchronously update every subscribed source/projection
  -> publish authoring availability at the committed revision
```

Abort, preview, validation rejection, and semantic no-op do not commit, publish, or advance the persisted revision.
The coordinator verifies that a commit revision immediately follows the last coordinator-owned revision; a gap indicates a bypassing writer and faults the live runtime.

Failure before commit returns the operation's ordinary typed error and leaves the previous availability intact.
Once commit succeeds, the mutation is irrevocably committed.
If enqueueing or observer delivery fails afterward, the coordinator reports a committed-publication fault, enters a terminal `Faulted` authoring state, and rejects further live-runtime writes.
It never returns an ordinary pre-commit `Failed` result and never attempts an in-place reset whose ordering cannot be proven.
Reopening the runtime rebuilds projections from durable storage and establishes a fresh runtime instance.

### Ordered projection barrier

`LibraryChanges` remains the revision-ordering boundary for producers that finish on different threads.
For a callback-thread producer, dispatch and observer delivery complete synchronously.
For a worker producer, FIFO callback dispatch places the change delivery before the coroutine's callback continuation.

Publication completion runs only after every still-connected `LibraryChanges` observer has been invoked.
`TrackSourceCache`, `LiveTrackDetailProjection`, completion, workspace, and other authoring-relevant reducers therefore apply the change before the runtime advertises the new available revision.
If a future reducer becomes asynchronous, it must join an explicit acknowledgement barrier before availability can advance.

### Low-level and public access

`MusicLibrary` continues to expose read transactions and physical stores to core code.
`CoreRuntime::musicLibrary()` becomes read-only, and normal runtime consumers use `ao::rt::Library` roles and values.

Runtime implementation files that prepare mutations receive a narrow coordinator-owned session rather than constructing a transaction independently.
CLI behavior-bearing mutations use the runtime writer; deliberately offline inspection and repair use a separately named composition path with no live observers.

Build guardrails reject direct `WriteTransaction`, writable capability, and low-level store access from UIModel and normal frontend code.
Exceptions are narrow file/type allowlists rather than whole-directory or whole-coordinator exclusions.

## Alternatives

### Share the existing task mutex with `LibraryWriter`

This serializes callers but still allows commit and publication to be performed independently and leaves writable authority broadly constructible.
It is insufficient as the final boundary.

### Rely only on LMDB's single-writer lock

LMDB protects physical consistency but does not own application admission, maintenance state, semantic changes, projection ordering, or post-commit fault classification.

### Recover publication failures with an inferred reset

A reset after an unknown observer failure requires proving which projections already applied which revision and how the reset itself is ordered.
Stopping further authoring and rebuilding on runtime reopen is smaller and mechanically safe.

### Lock every process that opens the library

An all-open exclusive lease would also hide stale read-only dictionary caches, but it would prevent useful read-only CLI access while an interactive application is running.
Writer exclusion and read-side cross-process coherence are separate contracts.

## Compatibility and migration

The database layout does not change.
The implementation migrates live write families to the coordinator and removes mutable storage access from public runtime/frontend seams.
No compatibility overload preserves an uncoordinated live-runtime write path.

Storage fixtures receive an explicit offline writable capability.
CLI commands either use the runtime facade or declare an offline administration composition before opening storage.

## Validation

- Every successful runtime commit publishes exactly one `LibraryChangeSet` with the same nonzero revision.
- Abort, preview, rejected admission, and no-op paths neither publish nor advance the revision.
- A post-commit enqueue or observer failure produces a committed-publication fault and permanently closes authoring for that runtime.
- Availability for revision `R` is observed only after all synchronous authoring-relevant projections have applied change `R`.
- Interactive writes are rejected throughout scan apply, import, relink, and identity backfill maintenance.
- Slow preparation runs without coordinator writer ownership or an LMDB write transaction.
- A second writable process fails to acquire the same database while the first writable runtime is alive.
- Boundary checks reject new runtime/frontend raw transaction construction outside explicit offline/test locations.
- Deterministic concurrency tests cover interactive/maintenance admission, multiple worker commits, observer failure, cancellation, and shutdown.
- Linux ThreadSanitizer and the full `./ao check` gate pass.

## Open questions

None.

## Promotion plan

- Update [library architecture](../architecture/library.md) with coordinator and writable-capability ownership.
- Update [runtime execution architecture](../architecture/runtime-execution.md) with maintenance admission and callback publication ordering.
- Update the [mutation specification](../spec/library/runtime/mutation.md), [task-execution specification](../spec/library/runtime/task-execution.md), and [change-publication specification](../spec/library/runtime/change-publication.md).
- Update the [library database reference](../reference/library/storage/database.md) only if the writer-lease path becomes a governed on-disk surface.
- Update development boundary checks and concurrency validation evidence.
