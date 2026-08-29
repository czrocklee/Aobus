---
id: decision.0015.sequence-live-runtime-library-writes
type: decision
status: accepted
domain: library
summary: Sequences every live-runtime library transaction and its publication through one coroutine lane.
---
# Decision 0015: sequence live-runtime library writes

## Context

Live library commits were already serialized across a wider interval than LMDB's native single-writer transaction: transaction, durable commit, change publication, replica application, observer delivery, availability delivery, and admission of the next writer.
That logical lane was represented indirectly by a physical writer mutex, a condition variable, background-writer reservation, publication barriers, Maintenance handoff, and callback-versus-worker branches.
The model preserved serializability, but a callback-executor caller could still pass its state check immediately before a background writer acquired the mutex and then block behind an unbounded scan transaction.
Workers could likewise occupy threads while waiting for writer or publication admission, and coordinated Closing had to reconcile several independent gates.

The application-concept-debloat review compared a complete sequencer with two smaller alternatives: callback-side `try_lock`, and `try_lock` plus coroutine-based Maintenance entry and exit.
The smaller approaches could contain the immediate UI stall at much lower implementation cost.
The accepted reason for the larger change was therefore not higher LMDB concurrency, throughput, or fewer source lines.
It was to make the already-serialized runtime write lifecycle one structural protocol: one transaction opener, visible FIFO ordering, suspension instead of executor-thread blocking, and one Closing quiescence point.
Aobus has three current frontends and no compatibility requirement for its in-process runtime mutation API, so review selected a clean cutover rather than paying for a mixed legacy/sequencer protocol.

Decision 0014 remains the rationale for item-evidence scan admission, optimistic preparation, and one atomic scan transaction.
This decision replaces only the writer-reservation and scheduler-window coordination that existed when Decision 0014 was accepted.

## Decision

Each live `ao::rt::Library` owns one private write sequencer inside `LibraryWriteLane`.
That sequencer owns the runtime's sole `WritableMusicLibrary` and is the only live-runtime authority allowed to open a `WriteTransaction`.
Explicit offline scan/YAML composition and lower core-library fixtures remain outside the live runtime.

Interactive commands, exact previews, scan apply, audio-identity write-back, YAML import preview/apply, and Maintenance entry/exit use one FIFO command lane.
Queue and publication waits suspend their coroutine and occupy no executor thread.
Once a command receives the active turn, it resumes on a worker and invokes a normal non-coroutine transaction kernel; no native transaction spans a suspension point.
An effective command retains the lane through durable commit and revision settlement, including replica and observer delivery plus the revision's applicable availability notification.
No-op, preview, and pre-commit failure paths abort the transaction and release the lane without publication.

Public live `LibraryCommands` mutators and previews return owning `async::Task<Result<T>>` values.
Inputs that survive submission are copied or moved into command-owned storage; caller spans, views, paths by reference, transaction-bound values, and raw owner pointers do not cross the asynchronous boundary.
Track/List authoring maps transient lane contention to non-terminal `Busy`; ordinary `Result` commands use `Error::Code::ResourceBusy`.
`Unavailable` remains a logical admission/lifetime result, and `Stale` remains evidence invalidation.
At most one interactive command and one background mutation are outstanding, which bounds the lane without a general mailbox limit.

Maintenance entry and exit are sequenced control commands.
Revision settlement for a Maintenance-owned commit is distinct from workflow settlement: the command first settles its revision while mode remains `Maintenance`; the outer workflow then exits Maintenance, delivers final `Available`, and only then completes.
Exact previews retain the same lane and aborted-write implementation as their committing counterpart rather than using an advisory read approximation.

After durable commit, caller cancellation cannot reinterpret the transaction as rolled back.
The internal publication terminal is either `Published` or `RetiredByClosing`; a still-live waiter may observe the latter through the existing `OperationCancelled` control-flow channel without implying rollback.
Correctness-critical settlement state is prepared before native commit, and an ordinary post-commit settlement failure remains fatal.

Closing seals command admission, retires queued requests outside the state lock, and requests cooperative stop from active pre-transaction work.
It advances the active command to a safe terminal phase before unconditionally sealing `LibraryChanges`.
An unclaimed Maintenance availability delivery or a queued publication may be retired; an already-running callback delivery completes normally.
Closing waits for the lane to become idle before callback resumption and workers are stopped, so no separate submission barrier or writer-owner branch remains.

A build guard rejects another live-runtime `WritableMusicLibrary::writeTransaction()` opener outside the sequencer-owned boundary.
The lower process writer gate remains as defense for offline composition and core-library use, not as the normal runtime scheduling mechanism.

## Alternatives considered

- **Callback-side `try_lock`.** This would have removed the known callback stall with a small local change, but left worker blocking, direct transaction owners, publication admission, Maintenance, and Closing split across existing mechanisms.
- **Callback-side `try_lock` plus coroutine Maintenance.** This would also have removed most Maintenance dispatch/retry plumbing at much lower cost. It still left runtime transaction and publication ownership distributed and did not provide one Closing quiescence protocol.
- **Mixed sequencer and direct-writer migration.** Rejected because queue state and legacy mutex admission would coexist, preserve the original race surface, and require a temporary protocol with no product compatibility value.
- **An asynchronous mutex around direct writers.** Rejected because it serializes only native transaction ownership, not publication, availability, Maintenance, and Closing settlement.
- **All writes on the callback executor.** Rejected because scan and import transactions are not item- or byte-bounded and would freeze GTK, TUI, CLI, and WinUI control loops.
- **A dedicated writer thread.** Rejected because the existing worker pool can execute the synchronous kernel, while a suspended command needs no thread and LMDB still permits only one writer.
- **Advisory read-only previews.** Rejected because they would duplicate mutation validation, could disagree with commit, and would still require stale-result presentation.
- **A reusable actor framework.** Rejected because only the library runtime needs this specialized transaction/publication/Maintenance/Closing lane.
- **Nested transactions or partial scan commits.** Rejected because they do not solve executor blocking or settlement and would change scan atomicity, revision traffic, and relink recovery.

## Consequences

Callback executors no longer block on a library writer mutex, and ordinary queue/publication waits no longer occupy worker threads.
Every live transaction has one visible owner and order, reentrant publication/availability mutation is rejected at submission, and Closing has one lane-idle terminal condition.
The normal runtime no longer needs a writer mutex, writer condition variable, writer kinds, background-writer reservation, or a separate publication-submission barrier.

The cost is a broad asynchronous API and lifetime migration across runtime, UIModel, CLI, and GTK.
Small uncontended writes add worker scheduling, command/event state, a callback publication turn, and owning-input costs.
All exact previews also use the asynchronous lane.
Large atomic scans still cause head-of-line contention; the sequencer changes how callers wait or are rejected, not the duration or rollback cost of that transaction.
The implementation is structurally clearer but is not expected to reduce total source lines or increase maximum write throughput.

Ordinary commands now complete after revision settlement even when they originated on a worker; this is stronger than the former foreign-worker behavior, which could return after publication submission.
UIModel sessions retain pending state across the await, reject a second local submission, and preserve drafts/bindings on `Busy`.
GTK binds one-shot task completions to owner lifetime, while multi-stage workflows keep explicit coroutines.
CLI preserves a synchronous process-command surface by pumping its existing callback loop rather than blocking an executor needed by the command.

## Current authorities

- [Library architecture](../architecture/library.md)
- [Runtime execution architecture](../architecture/runtime-execution.md)
- [Library access and mutation](../spec/library/runtime/mutation.md)
- [Library change publication](../spec/library/runtime/change-publication.md)
- [Library task execution](../spec/library/runtime/task-execution.md)
- [Library scan and audio identity](../spec/library/runtime/scan-and-identity.md)
- [Library YAML transfer](../spec/library/runtime/yaml-transfer.md)

## Supersession

None.
