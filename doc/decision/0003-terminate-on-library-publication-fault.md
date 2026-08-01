---
id: decision.0003.terminate-on-library-publication-fault
type: decision
status: accepted
domain: library
summary: Terminates a live process when mandatory publication fails after durable library commit instead of exposing a recoverable Faulted or committed-but-unpublished state.
---
# Decision 0003: terminate on live library publication fault

## Context

The July 2026 error-handling remediation found that a native library commit and its mandatory in-process publication cannot share one rollback boundary.
Early designs exposed terminal `Faulted` plus reopen, then expanded into mutation receipts, publication tickets, acknowledgement waiters, partial-commit summaries, health axes, and frontend recovery branches.
Those protocols made rare infrastructure failures recoverable in theory, but required every writer, task, and frontend to understand a database revision that was durable while the live replica might be stale.

Aobus is a single-user desktop music application whose runtime state is rebuilt from the database at process start.
The product does require an ordinary task return to mean that committed state has reached the mandatory runtime replica; it does not require in-process recovery from executor or publication-invariant failure.

## Decision

Pre-commit validation, storage, conflict, and cancellation failures retain their existing typed channels.
After native commit, the coordinator admits and completes mandatory publication before the operation can release maintenance or return success.
An invariant, admission, or delivery failure from that commit through publication completion while the runtime is live is an infrastructure fault: Aobus makes a best-effort diagnostic and terminates the process.

There is no public `Faulted` authoring state, reopen command, publication receipt, committed-but-unpublished task result, or recovery state machine.
The next process open validates the durable database and reconstructs the runtime replica.

Coordinated Closing is the sole exception.
It seals writer and task admission before callback admission closes, retires publication or maintenance-finalization callbacks that have not begun, and wakes blocked workers before joining them.
Retired work produces no task outcome; an already-running publication remains `noexcept` and completes.

## Alternatives considered

### Expose terminal Faulted and reopen only the library

Rejected because every bound source, view, selection, playback owner, and frontend would need a degraded-session contract while the process retained a stale runtime graph.
For the current product, restarting the process is operationally equivalent for the user and has a much smaller correctness surface.

### Return a typed commit/publication receipt

Rejected because it makes all callers aggregate durability, publication health, cancellation, and partial-batch outcomes even though no current workflow can safely continue against an unpublished revision.

### Treat publication as optional or catch callback exceptions

Rejected because the single replica is mandatory runtime state, not presentation.
Replica and notification callbacks are already `noexcept`; catching optional presentation work belongs at those optional boundaries and does not make revision admission or mandatory delivery recoverable.

### Block on a publication acknowledgement object

Rejected because the production callback executor already supplies the required order.
An owner-thread publication runs inline; a foreign producer admits publication before the same operation admits its final callback continuation into one FIFO.

## Consequences

- An ordinary maintenance-task return remains a materialization barrier without a public protocol type.
- Recoverable pre-commit errors remain distinct from post-commit infrastructure faults.
- A live publication fault ends the whole process, which has wider session impact than reopening only the library and may interrupt unrelated presentation or playback state.
- Durable library state survives and is reconstructed on the next validated open; optional in-memory state since its last checkpoint may not.
- Diagnostics at this terminal boundary are best effort and do not promise allocation-free evidence.
- Shutdown needs explicit close-before-runtime-stop ordering and weak callback lifetime gates.
- Fatal admission and allocation failures do not require dedicated fault-injection or death-test infrastructure.

## Current authorities

- [Library architecture](../architecture/library.md)
- [Runtime execution architecture](../architecture/runtime-execution.md)
- [Library change publication](../spec/library/runtime/change-publication.md)
- [Library mutations](../spec/library/runtime/mutation.md)
- [Library task execution](../spec/library/runtime/task-execution.md)
- [Library YAML transfer](../spec/library/runtime/yaml-transfer.md)

## Supersession

Not superseded.
