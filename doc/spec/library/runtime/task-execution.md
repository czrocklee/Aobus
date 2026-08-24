---
id: library.task-execution
type: spec
status: current
domain: library
summary: Defines executor affinity, mutation serialization, progress finalization, outcomes, and cancellation for long-running library tasks.
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

The maintenance guard survives worker hops.
Every ordinary value, Error, exception, or cancellation path performs one non-cancellable return to the callback executor and explicitly finishes maintenance there.
Guard destruction is only an idempotent fallback; it dispatches owner-affine finalization when the live runtime still exists and becomes a no-op after coordinated Closing retires the guard.
Once a maintenance transaction may have committed, the same non-cancellable return is ordered after mandatory publication, so publication and cleanup cannot be skipped.

## Progress and outcome

Task progress uses owner-local operational channels on `LibraryTaskService`.
Progress is best effort and does not constitute a committed state transition.
Each operation keeps at most one progress-delivery callback queued; while that callback is pending, a newer update replaces the pending value for the same phase.
Phase transitions remain ordered, so consumers observe each entered phase and the newest accepted state within that phase without per-file event flooding.
`onProgressFinished()` is a status-free pulse used only to clear presentation progress.
It carries no outcome, Error, cancellation state, count, or message.
The awaited `Task<Result<T>>` value and the ordinary cancellation/exception channel are the sole operation outcome; the task-specific caller owns final wording and payload presentation.

A progress conversation begins only after the method's initial cancellable callback-executor admission succeeds.
Cancellation before that admission emits neither progress nor a finished pulse.
Once admitted, each TaskService method that may publish progress emits exactly one finished pulse on its ordinary value, Error, exception, or cancellation path after callback-owner cleanup while the runtime owner remains live, whether or not it emitted a progress update.
The callback executor's FIFO delivers any final queued progress update before that conversation's finished pulse.
Scan-plan build and scan apply are independent progress conversations and therefore emit independent pulses.
Resource loading, YAML export, and the import operations currently publish no task progress and emit no finished pulse.

Committed content or manifest changes publish only through the revisioned changeset channel.
Neither the finished pulse nor an awaited-task caller refreshes runtime replicas, caches, sources, or projections.

## Cancellation

Every task uses its stop token at worker/callback executor hops, but only operations with a synchronous stop-token surface support cancellation while their core work is running.

| Operation | In-operation cancellation |
|---|---|
| YAML import/export | None after synchronous transfer work begins. |
| Scan plan build | None during the synchronous filesystem walk. |
| Scan plan apply | Cooperative between items and fingerprint chunks; cancellation aborts the plan transaction. |
| Identity backfill | Cooperative between hash chunks; completed hashes in the current batch may be flushed. |

Lifetime cancellation unwinds the coroutine and prevents post-cancellation access to destroyed borrowed owners.
Business code does not reinterpret cancellation as a generic recoverable error or encode it in a successful value.
Pre-admission cancellation has no progress-channel side effect; post-admission cancellation follows the admitted conversation's cleanup contract.
Once scan apply or identity backfill has entered maintenance, task plumbing returns to the callback executor without consulting the stop token, finishes maintenance, emits the status-free progress pulse, and then propagates `OperationCancelled`.
The same cleanup occurs whether cancellation is observed as stop-token state or escapes from worker code as an exception.

## Failure behavior

Worker-side `Result` failures resume as `Result` failures on the callback executor.
If an unexpected exception escapes worker execution, task plumbing carries and rethrows it on the callback side after required maintenance and progress cleanup.
Failure before commit releases maintenance without advancing the library revision.
An ordinary successful task return is a materialization barrier: every revision committed by that operation has completed mandatory publication before maintenance is released.
Failure from native commit through mandatory publication in a live runtime is an infrastructure fault and terminates; task code does not reinterpret durable mutation as an uncommitted `Result` failure.
Coordinated Closing may retire a not-yet-running publication or finalization callback and produces no task outcome.
It may likewise retire an admitted progress conversation before its callback-owner finalization; that owner-lifetime boundary emits no synthetic finished pulse.
Optional progress and per-item failure callbacks are contained at their local adapter boundary and logged; they cannot replace the task outcome.

## Implementation map

- [`LibraryTaskService.h`](../../../../app/include/ao/rt/library/LibraryTaskService.h) defines the async task surface.
- [`LibraryTaskService.cpp`](../../../../app/runtime/library/LibraryTaskService.cpp) owns executor hops, coordinator composition, and notification adaptation.
- [`LibraryMutationService.h`](../../../../app/runtime/library/LibraryMutationService.h) owns maintenance admission and bounded write sessions.
- [`LibraryTaskEvents.h`](../../../../app/include/ao/rt/library/LibraryTaskEvents.h) defines the progress payload independently of the task-operation surface.

## Test map

- [`LibraryTaskServiceTest.cpp`](../../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) proves worker/callback affinity, bounded progress coalescing and terminal ordering, maintenance admission, errors, status-free progress finalization, callback containment, cancellation cleanup, maintenance release, and the mandatory post-commit barrier.
- [`AudioIdentityIndexerTest.cpp`](../../../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp) proves concurrent fingerprinting and bounded write-back behavior.

## Related documents

- [Runtime execution architecture](../../../architecture/runtime-execution.md)
- [Library change publication](change-publication.md)
- [Library scan and audio identity](scan-and-identity.md)
- [Library YAML transfer](yaml-transfer.md)
