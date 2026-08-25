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
- Import and import preview enter one exclusive coordinator Maintenance interval through a sequenced asynchronous control command before slow preparation begins.
- Scan apply and identity backfill acquire a background-task lease on the callback executor. That lease serializes scan, backfill, and import preparation without changing authoring availability.
- Maintenance closes interactive admission for the whole import interval but does not itself hold an LMDB write transaction.
- YAML export and scan-plan construction do not enter maintenance or acquire a background-task lease. Scan planning copies one short LMDB manifest snapshot and closes it before filesystem work.
- Parsing, filesystem walking, media interpretation, and identity fingerprinting run without transaction ownership.
- Import submits one generation-bound Maintenance mutation for preview or prepared apply; scan apply submits one background command; identity backfill submits one background command per bounded write-back batch.
- Every effective Maintenance, background, or interactive mutation commits and reaches ordered revision settlement through the same command lane.

## Background tasks and maintenance states

One background-task lease excludes another scan, identity backfill, or YAML import preparation.
The lease survives worker hops, emits no availability notification, and leaves interactive mutation admission open.
Each background write phase submits one owning command to the private per-library FIFO lane.
The command waits by coroutine suspension; after grant it resumes on a worker and runs its complete transaction kernel without suspension.
Once a background mutation is queued or active, a later Track/List authoring command returns non-terminal `Busy` and another ordinary mutation or exact preview returns `ResourceBusy` instead of waiting.
An interactive command accepted before that background intent retains its FIFO turn.
Contention alone does not change `LibraryAuthoringAvailability` or emit a notification.

`LibraryAuthoringAvailability` identifies active YAML import maintenance as `Import`.
Beginning another task or any interactive command while maintenance is active returns `InvalidState`/`Unavailable` through its owning API.
YAML import also holds the background-task lease, so scan or backfill cannot prepare concurrently with it.

Maintenance entry and exit are lane control commands rather than destructor-driven callback handoffs.
Entry waits behind commands accepted earlier, changes mode to `Maintenance`, dispatches that availability to the callback executor, and returns a generation-bound guard only after observers finish.
While the guard is active, only matching Maintenance mutations are admitted.
A committed Maintenance mutation reaches revision settlement while mode remains `Maintenance`.
The outer workflow then awaits guard completion, whose exit command changes mode to `Available`, delivers final availability, and establishes workflow settlement before the public Task returns.
Guard destruction remains an idempotent lifetime fallback; coordinated Closing may retire an unclaimed control delivery and wake its waiter.

The background-task lease uses the same lifetime-safe owner anchor but needs no callback-affine availability transition when it finishes.
Scan and backfill return to the callback executor for workflow finalization, release the lease, and emit their progress-finished pulse only after every committed command reached revision settlement.

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
| Scan plan build | Cooperative while copying the manifest, walking entries, reporting progress, matching moves, and hashing candidates. |
| Scan plan apply | Cooperative between items and fingerprint chunks; cancellation aborts the plan transaction. |
| Identity backfill | Cooperative between hash chunks; completed hashes in the current batch may be flushed. |

Lifetime cancellation unwinds the coroutine and prevents post-cancellation access to destroyed borrowed owners.
Business code does not reinterpret cancellation as a generic recoverable error or encode it in a successful value.
Pre-admission cancellation has no progress-channel side effect; post-admission cancellation follows the admitted conversation's cleanup contract.
Once scan apply or identity backfill has acquired its background-task lease, task plumbing returns to the callback executor without consulting the stop token, finishes the lease, emits the status-free progress pulse, and then propagates `OperationCancelled`.
The same cleanup occurs whether cancellation is observed as stop-token state or escapes from worker code as an exception.
Closing separately requests stop from active pre-transaction work and retires queued command requests.
Once native commit succeeds, caller cancellation cannot replace `Published` or internal `RetiredByClosing` with an ordinary pre-commit outcome.
An identity batch whose hashes were already submitted keeps the existing flush boundary: the accepted batch is completed rather than discarded solely because caller stop arrives while it waits for its turn.

## Failure behavior

Worker-side `Result` failures resume as `Result` failures on the callback executor.
If an unexpected exception escapes worker execution, task plumbing carries and rethrows it on the callback side after required lease, maintenance, and progress cleanup.
Failure before commit releases the owning guard or lease without advancing the library revision.
An ordinary successful task return is a materialization barrier: every revision committed by that operation has reached revision settlement before its guard or lease is released.
Failure from native commit through mandatory publication in a live runtime is an infrastructure fault and terminates; task code does not reinterpret durable mutation as an uncommitted `Result` failure.
Coordinated Closing may retire a queued command, a not-yet-running publication, or an unclaimed Maintenance control delivery and produces no synthetic task outcome.
It may likewise retire an admitted progress conversation before its callback-owner finalization; that owner-lifetime boundary emits no synthetic finished pulse.
Optional progress and per-item failure callbacks run inside task execution rather than an exception-containment boundary.
If one throws, required cleanup completes before that exception propagates and replaces the task outcome; a per-item failure is logged before its optional callback runs.

## Implementation map

- [`LibraryTaskService.h`](../../../../app/include/ao/rt/library/LibraryTaskService.h) defines the async task surface.
- [`LibraryTaskService.cpp`](../../../../app/runtime/library/LibraryTaskService.cpp) owns executor hops, coordinator composition, and notification adaptation.
- [`LibraryMutationService.h`](../../../../app/runtime/library/LibraryMutationService.h) owns background-task exclusion, the command lane, Maintenance control commands, revision settlement, and Closing.
- [`LibraryTaskEvents.h`](../../../../app/include/ao/rt/library/LibraryTaskEvents.h) defines the progress payload independently of the task-operation surface.

## Test map

- [`LibraryTaskServiceTest.cpp`](../../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) proves worker/callback affinity, bounded progress coalescing and terminal ordering, background-task exclusion, interactive authoring during scan preparation and after background commit, maintenance admission, errors, status-free progress finalization, callback exception propagation, cancellation cleanup, and the mandatory post-commit barrier.
- [`LibraryAuthoringTest.cpp`](../../../../test/unit/runtime/library/LibraryAuthoringTest.cpp) proves non-terminal contention behind a background command, worker-side execution, Maintenance ordering, active pre-transaction Closing cancellation, and later admission after release.
- [`AudioIdentityIndexerTest.cpp`](../../../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp) proves concurrent fingerprinting and bounded write-back behavior.

## Related documents

- [Decision 0015: sequence live-runtime library writes](../../../decision/0015-sequence-live-runtime-library-writes.md)
- [Runtime execution architecture](../../../architecture/runtime-execution.md)
- [Library change publication](change-publication.md)
- [Library scan and audio identity](scan-and-identity.md)
- [Library YAML transfer](yaml-transfer.md)
- [Decision 0014: admit scan plans by item evidence](../../../decision/0014-admit-scan-plans-by-item-evidence.md)
