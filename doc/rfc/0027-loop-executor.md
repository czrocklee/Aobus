---
id: rfc.0027.loop-executor
type: rfc
status: implemented
domain: runtime
summary: Introduced an owner-thread LoopExecutor and a CLI task pump that returns worker callbacks to the CLI thread.
depends-on: none
---
# RFC 0027: Loop executor for non-toolkit hosts

## Disposition

Implemented on 2026-07-15 with the narrow owner-driven design described below.

`async::LoopExecutor` now supplies the same serialized callback-domain contract as the GTK and TUI executors without owning a toolkit event loop.
`CliRuntime::runTask()` drives that executor until an asynchronous command reaches terminal completion, so worker-to-callback continuations return to the CLI invocation thread.
The production `ImmediateExecutor` and duplicate queued test implementation were removed.

The [runtime execution architecture](../architecture/runtime-execution.md) and [CLI execution specification](../spec/cli/execution.md) own current behavior.
Those authorities supersede this proposal; this RFC retains the problem, the deliberately small design, and the rejected broader alternatives.

## Problem

The executor contract requires `dispatch()` to run inline only on the owning thread and otherwise enqueue work for that thread.
Before this RFC, CLI supplied `ImmediateExecutor`, which reported every thread as current, dispatched inline on the caller, and kept an unsynchronized deferred queue.

That implementation was adequate only for a truly single-threaded test.
`async::Runtime` owns worker threads, and `resumeOnCallbackExecutor()` can dispatch a coroutine continuation from one of those workers.
With `ImmediateExecutor`, that continuation stayed on the worker and could enter executor-affine runtime state concurrently with the CLI thread.
Concurrent `defer()` calls could also race on the inline executor's queue.

The `lib fingerprint --pending` command already runs identity work on the runtime worker pool and synchronously waits for its result.
Blocking on its future alone was not a general solution: any operation that needed a callback-executor hop before terminal completion would wait for a callback that the CLI never drove.

GTK and TUI did not have this gap because their executors wake an existing toolkit loop.
The missing primitive was a small executor that reused the same synchronized queue but let a non-toolkit owner explicitly run its turns.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0011](0011-executor-affine-reporting-feed.md), [RFC 0012](0012-structured-async-fault-diagnostics.md).

The implementation does not depend on those RFCs being implemented.
It provides the serialized CLI callback domain that future mutation publication, reporting delivery, and terminal diagnostics can reuse.

## Goals

- Make CLI callback dispatch obey the shared owner-thread executor contract.
- Reuse the existing synchronized queue and turn semantics.
- Let a non-toolkit owner wait for one turn or run one already-ready turn.
- Let synchronous CLI adapters drive an asynchronous task without polling a future or deadlocking on a callback hop.
- Preserve task results and exceptions at the synchronous command boundary.
- Quiesce worker producers before draining callbacks during CLI teardown.
- Remove production and test implementations that duplicated or contradicted the executor contract.

## Non-goals

- Add a general event-loop framework, request state machine, deadline API, capacity policy, or close/admission protocol.
- Give CLI a dedicated callback thread.
- Change GTK or TUI event-loop integration.
- Make every runtime service internally thread-safe.
- Add interactive playback or another long-lived CLI mode.
- Replace deterministic test schedulers whose one-step or forced-queue behavior is intentionally different from production.

## Implemented design

### Shared queued core

`QueuedExecutorBase` remains the common multi-producer, owner-drained implementation used by GTK, TUI, and `LoopExecutor`.
It owns:

- the constructing thread identity;
- the mutex-protected pending and current-turn queues;
- owner-inline versus foreign-queued `dispatch()` behavior;
- always-later `defer()` behavior;
- non-reentrant turn extraction; and
- wake coalescing for a pending burst.

Each concrete executor only supplies its wake mechanism and callback exception policy.
The base source files match the `QueuedExecutorBase` type name so it is not confused with the deterministic test `QueuedExecutor`.

### Loop executor

`LoopExecutor` derives from `QueuedExecutorBase` and uses one `std::binary_semaphore` as its wake signal.
Wake coalescing guarantees that the queue needs at most one outstanding signal.
Foreign producers enqueue and release that signal; they never execute the callback.

The owner has two operations:

- `runOneTurn()` waits for a signal and executes one extracted turn; and
- `runReadyTurn()` executes one signaled turn without waiting and reports whether one was ready.

Tasks admitted while a turn is running remain in the pending queue and produce a later signal when the current turn ends.
If a callback throws, that exception reaches the driver while callbacks behind it remain ready for a later turn.
Only the constructing owner thread may run turns.
The executor has no lifecycle state machine or generic `runUntil` API; operation ownership remains with the host.

### CLI task boundary

`CliRuntime` constructs one `LoopExecutor`, transfers its ownership to `CoreRuntime`, and retains a non-owning pointer for driving turns.
CLI commands still execute synchronously on the constructing thread.

`CliRuntime::runTask()` spawns a small wrapper coroutine that:

1. installs a scope-finalizer that dispatches a terminal completion marker to the callback executor;
2. awaits the supplied task;
3. dispatches the marker from that finalizer on both normal return and exception unwinding; and
4. lets the CLI owner run turns until that marker is delivered.

The terminal marker matters even when the task completes without an ordinary callback hop: it guarantees that a blocked `runOneTurn()` receives a wake instead of relying on future polling.
The value overload stores a non-void result across that same boundary and then returns it to the command adapter.
The spawned future retains a task failure and rethrows it on the CLI owner thread.
If a callback turn throws, `runTask()` retains the first callback exception, continues pumping through terminal completion, and consumes the spawned future before leaving the boundary.
A task failure remains primary and the callback failure is reported; otherwise the callback failure is rethrown on the invocation thread.
This prevents command-owned inputs from unwinding while their worker task still uses them.

`lib fingerprint --pending` uses this boundary rather than calling `future.get()` directly.
Output, progress, and failure callbacks retain their existing command behavior while future callback hops preserve CLI-thread affinity.

### Teardown

`CliRuntime` first requests worker stop and joins the runtime worker pool.
It then runs every already-ready callback turn while `CoreRuntime` and its callback targets are still alive.
An unexpected callback exception during this drain is sent to the runtime's unhandled-exception path.
Only after producer quiescence and draining does CLI destroy `CoreRuntime` and its owned executor.

This ordering is sufficient for the current synchronous CLI lifetime.
It does not claim that an empty queue proves arbitrary external producers are quiescent; a future long-lived CLI host must own those producers explicitly.

### Test executors

The production `ImmediateExecutor` was removed.
Tests now choose among distinct contracts:

- `InlineExecutor` is a test-only double that collapses dispatch and defer when turn and cross-thread behavior are out of scope;
- `ManualExecutor` retains one-task manual stepping for tests that need that exact control; and
- test `QueuedExecutor` forces all delivery through explicit turns, but delegates its real queue and wake semantics to production `LoopExecutor` and adds only bounded observation helpers.

The old file-local `ManualQueuedExecutor` and its separate queue tests were replaced by direct `LoopExecutor` tests.
Runtime worker-to-callback coverage now proves that the continuation returns to the owner thread rather than merely completing on a different thread.

The council tool also supplies `LoopExecutor` to `async::Runtime` instead of the removed inline executor.
Its current tasks use only the worker pool and do not perform callback-executor hops; introducing such a hop requires an explicit owner pump at that composition boundary.

## Alternatives

### Protect the inline executor with a mutex

A mutex could protect its deferred queue but could not make foreign-thread inline `dispatch()` execute on one owner thread.
It would preserve the central affinity bug.

### Add a dedicated callback thread

A dedicated thread would require every synchronous CLI runtime access to marshal onto that thread and return a result.
The invocation thread is already the natural owner, so an explicit pump is smaller.

### Add `runUntil`, deadlines, admission states, and close policies

Those operations require cancellation and ownership decisions that belong to the operation or host.
The current bug needs only one blocking turn, one non-blocking turn, and a CLI-owned terminal marker.

### Poll the task future

Polling introduces timing and still does not deliver callback-executor work.
Waiting only on the future deadlocks when completing the future requires an unpumped callback hop.

### Promote a test executor unchanged

The test `QueuedExecutor` intentionally queues owner-thread dispatch and exposes queue observations; the old file-local executor did not actually wait for wake signals.
Neither was the production contract.
Delegating test delivery to `LoopExecutor` preserves their useful test controls without promoting test-only semantics.

## Compatibility and migration

CLI syntax, output shapes, exit codes, and persisted formats do not change.
The observable correction is thread affinity: callbacks submitted by worker or provider threads now run on the CLI owner.

Production code no longer includes `ImmediateExecutor`.
Tests that only need synchronous behavior use the explicitly test-only inline double; tests that exercise turns or foreign producers use `LoopExecutor` or the controlled queued adapter.

## Validation

- Owner `dispatch()` remains inline; foreign `dispatch()` runs only on the owner.
- Concurrent producers are delivered once through one coalesced ready turn.
- FIFO order and later-turn behavior hold for bursts, nested defer, work admitted during a turn, and nested pumping.
- A throwing turn preserves both newly deferred work and callbacks already queued behind the failure for later turns.
- A runtime coroutine resumes from a worker onto the constructing owner thread.
- `CliRuntime::runTask()` completes a worker-to-owner round trip and rethrows worker failure on the owner.
- A callback failure cannot make `runTask()` leave before its supplied task reaches terminal completion.
- CLI teardown drains a callback queued by a foreign producer before releasing callback targets.
- Player coverage exercises a real loop while synchronous owner callbacks remain inline and foreign callbacks require pumping.
- The default full gate and the AddressSanitizer gate pass. Targeted ThreadSanitizer coverage for `LoopExecutor` and `CliRuntime` also passes; the full GTK ThreadSanitizer gate remains blocked by a pre-existing Cairo race reproduced with the pre-change binary.

## Open questions

None for the implemented boundary.
A future non-toolkit host that performs callback hops must provide its own owner pump and producer-lifetime policy.

## Promotion plan

The implemented current behavior is owned by:

- [Runtime execution architecture](../architecture/runtime-execution.md)
- [CLI execution specification](../spec/cli/execution.md)
- [Audio execution and concurrency specification](../spec/playback/audio-execution.md)
- [Runtime and asynchronous testing guidance](../development/test/runtime-and-async.md)

No reference document or ADR was added because the implementation introduces no persisted format, command syntax, or alternative architectural authority.
