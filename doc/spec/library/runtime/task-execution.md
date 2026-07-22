---
id: library.task-execution
type: spec
status: current
domain: library
summary: Defines executor affinity, mutation serialization, progress, completion, and cancellation for long-running library tasks.
---
# Library task execution

## Scope

This specification defines the common behavior of `LibraryTaskService` operations: YAML import/export, scan planning/application, and audio-identity backfill.
Operation-specific data semantics remain in their owning specifications.

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md) and refines the process-wide [runtime execution architecture](../../../architecture/runtime-execution.md).
`LibraryTaskService` is public under `app/include/ao/rt/library/`, implemented in `app/runtime/library/`, and coordinates `ao::async::Runtime` plus core library facilities without making the worker pool a second state owner.

## Invariants

- Filesystem, parsing, hashing, and other long work runs on the shared worker pool.
- Returned `Task<Result<T>>` values resume their caller on the callback executor.
- Recoverable lower-layer failures remain `Result` errors across executor hops.
- Unexpected invariant exceptions propagate to the callback boundary rather than being converted into external-data errors.
- Import, import preview, scan apply, and identity backfill enter one exclusive coordinator maintenance interval on the callback executor before slow preparation begins.
- Maintenance closes interactive admission for the whole interval but does not itself hold coordinator writer ownership or an LMDB write transaction.
- YAML export and scan-plan construction rely on one LMDB read snapshot and do not enter maintenance.
- Parsing, filesystem walking, media interpretation, and identity fingerprinting run without writer ownership.
- Import and scan apply acquire one maintenance mutation for prepared apply; identity backfill acquires one per bounded write-back batch.
- Every effective maintenance mutation commits and completes ordered change publication through the same coordinator as an interactive command.

## Maintenance states

`LibraryAuthoringAvailability` identifies active maintenance as `Import`, `ScanApply`, or `AudioIdentityBackfill`.
Beginning another maintenance operation or any interactive command while maintenance is active returns `InvalidState`/`Unavailable` through its owning API.

The maintenance guard survives worker hops and releases availability on every pre-commit success, error, exception, or cancellation path.
Once a maintenance transaction may have committed, the coroutine returns to the callback executor without a cancellable hop so publication and cleanup cannot be skipped.

## Progress and completion

Task progress and completion use the operational channels in `LibraryChanges`.
Progress is best effort and does not constitute a committed state transition.
The terminal event distinguishes `Succeeded`, `CompletedWithIssues`, `Failed`, and `Cancelled`; every status clears active progress, while presentation may announce success only for `Succeeded`.
Its affected count describes the operation-specific completed item count and is zero when no useful item completed.

Committed content or manifest changes publish through the revisioned changeset channel separately from task completion.

## Cancellation

Every task uses its stop token at worker/callback executor hops, but only operations with a synchronous stop-token surface support cancellation while their core work is running.

| Operation | In-operation cancellation |
|---|---|
| YAML import/export | None after synchronous transfer work begins. |
| Scan plan build | None during the synchronous filesystem walk. |
| Scan plan apply | Cooperative between items and fingerprint chunks; cancellation aborts the plan transaction. |
| Identity backfill | Cooperative between hash chunks; completed hashes in the current batch may be flushed. |

Lifetime cancellation unwinds the coroutine and prevents post-cancellation access to destroyed borrowed owners.
Business code does not reinterpret cancellation as a generic recoverable error.
After progress has been published, task plumbing emits a `Cancelled` terminal event before propagating `OperationCancelled`.

## Failure behavior

Worker-side `Result` failures resume as `Result` failures on the callback executor.
If an unexpected exception escapes worker execution, task plumbing carries and rethrows it on the callback side after required task notification cleanup.
Failure before commit releases maintenance without advancing the library revision.
Failure while enqueueing or delivering a committed revision leaves the coordinator terminally `Faulted`; task code does not reinterpret that durable mutation as an uncommitted `Result` failure.

## Implementation map

- [`LibraryTaskService.h`](../../../../app/include/ao/rt/library/LibraryTaskService.h) defines the async task surface.
- [`LibraryTaskService.cpp`](../../../../app/runtime/library/LibraryTaskService.cpp) owns executor hops, coordinator composition, and notification adaptation.
- [`LibraryMutationService.h`](../../../../app/runtime/library/LibraryMutationService.h) owns maintenance admission and bounded write sessions.
- [`LibraryChanges.h`](../../../../app/include/ao/rt/library/LibraryChanges.h) defines progress and completion channels.

## Test map

- [`LibraryTaskServiceTest.cpp`](../../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) proves worker/callback affinity, maintenance admission, errors, progress, cancellation before import admission, and mandatory post-commit completion.
- [`AudioIdentityIndexerTest.cpp`](../../../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp) proves concurrent fingerprinting and bounded write-back behavior.

## Related documents

- [Runtime execution architecture](../../../architecture/runtime-execution.md)
- [Library change publication](change-publication.md)
- [Library scan and audio identity](scan-and-identity.md)
- [Library YAML transfer](yaml-transfer.md)
