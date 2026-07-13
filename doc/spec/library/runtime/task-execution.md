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
- YAML import and scan application serialize through the service mutation mutex.
- YAML export and scan planning rely on one LMDB read snapshot and do not take the mutation mutex.
- Identity fingerprinting runs without the mutation mutex and takes it only for each bounded write-back transaction.

## Progress and completion

Task progress and completion use the operational channels in `LibraryChanges`.
Progress is best effort and does not constitute a committed state transition.
Completion count describes the operation-specific completed item count and is zero when the task cannot begin or fails before useful completion.

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

## Failure behavior

Worker-side `Result` failures resume as `Result` failures on the callback executor.
If an unexpected exception escapes worker execution, task plumbing carries and rethrows it on the callback side after required task notification cleanup.

## Implementation map

- [`LibraryTaskService.h`](../../../../app/include/ao/rt/library/LibraryTaskService.h) defines the async task surface.
- [`LibraryTaskService.cpp`](../../../../app/runtime/library/LibraryTaskService.cpp) owns executor hops, mutation serialization, and notification adaptation.
- [`LibraryChanges.h`](../../../../app/include/ao/rt/library/LibraryChanges.h) defines progress and completion channels.

## Test map

- [`LibraryTaskServiceTest.cpp`](../../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) proves worker/callback affinity, errors, progress, completion, and cancellation adaptation.
- [`AudioIdentityIndexerTest.cpp`](../../../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp) proves that fingerprinting does not hold the mutation mutex.

## Related documents

- [Runtime execution architecture](../../../architecture/runtime-execution.md)
- [Library change publication](change-publication.md)
- [Library scan and audio identity](scan-and-identity.md)
- [Library YAML transfer](yaml-transfer.md)
