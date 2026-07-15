---
id: rfc.0027.serialized-headless-callback-executor
type: rfc
status: draft
domain: runtime
summary: Proposes a thread-safe owner-thread executor and explicit pump contract for serialized callback delivery in CLI and other headless runtime hosts.
depends-on: none
---
# RFC 0027: Serialized headless callback executor

## Problem

The runtime architecture treats the callback executor as one serialized application-control domain.
`Executor::dispatch()` is documented as thread-safe: work runs immediately on the owning thread or is enqueued and wakes that thread.
GTK and TUI provide event-loop executors with an owner thread and synchronized cross-thread queues.

CLI instead constructs `ImmediateExecutor`.
It reports `isCurrent() == true` on every calling thread, executes `dispatch()` inline, and stores deferred work in an unsynchronized `std::deque` guarded only by a plain boolean.
It preserves nested FIFO behavior for one thread, but it does not implement the process-wide executor contract when a worker, audio/provider callback, or other producer calls it concurrently.

This is not merely an inaccurate assertion.
Runtime services use `isCurrent()` to enforce single-writer state and subscription lifetime.
`PlaybackService` explicitly notes that ImmediateExecutor hosts must remain effectively single-threaded because every thread appears current.
`async::Runtime::resumeOnCallbackExecutor()` dispatches from the worker pool, and library task/change publication can also return through the callback executor.
With `ImmediateExecutor`, such a continuation executes on the producer thread and can mutate state concurrently with the CLI command thread.

`defer()` is also vulnerable to data races and queue corruption when called from multiple producers.
Even without corruption, inline foreign-thread dispatch destroys the promised serialization and changes callback reentrancy according to which worker happens to complete.

Most CLI commands currently look synchronous, which masks the issue rather than establishing a contract.
As the CLI reuses more runtime tasks, diagnostics, and mutation publication, relying on "effectively single-threaded" becomes increasingly fragile.
Tests that use `ImmediateExecutor` can likewise pass behavior that a real event-loop executor would serialize differently.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0011](0011-executor-affine-reporting-feed.md), [RFC 0012](0012-structured-async-fault-diagnostics.md).

RFC 0003 mutation completion/publication must return through the serialized headless domain.
RFC 0011 feed delivery must use the same owner-thread executor.
RFC 0012 diagnostics may run directly at the terminal completion boundary because its injected handler is thread-safe and does not mutate application state; it therefore does not create a second callback authority.

## Goals

- Make headless `dispatch`, `defer`, and `isCurrent` obey the shared executor contract under multiple producers.
- Preserve one explicit owner thread for CLI/runtime mutable application state.
- Preserve non-reentrant deferred-turn FIFO behavior.
- Provide an explicit pump/wait API for headless hosts that have no toolkit event loop.
- Define admission close, drain/cancel, producer quiescence, and teardown behavior.
- Let synchronous CLI commands await asynchronous runtime completion without deadlock or foreign-thread callbacks.
- Provide deterministic tests for cross-thread dispatch, reentrancy, faults, cancellation, and shutdown.
- Keep the general executor abstraction independent of CLI command syntax and output.

## Non-goals

- Add interactive playback to the CLI.
- Make every runtime service internally mutex-protected or callable from arbitrary threads.
- Replace GTK or TUI executors.
- Create a general-purpose work-stealing scheduler; the existing worker pool remains the background-work owner.
- Promise that arbitrary blocking code on the owner thread can complete without pumping.
- Retain `ImmediateExecutor` as a production host when multiple producers can reach it.

## Proposed design

### Owner-thread headless executor

Add a `HeadlessExecutor` implementing `async::Executor` with:

- the thread id on which it is constructed/attached as its owner;
- a mutex-protected multi-producer queue;
- a condition variable or equivalent wake signal;
- an admission state;
- one non-reentrant drain owner; and
- explicit pump and shutdown operations available to the headless composition root.

`isCurrent()` compares the current thread with the recorded owner thread.
`dispatch(task)` executes inline only when called on the owner thread and outside any rule requiring a later turn; otherwise it enqueues and wakes the owner.
`defer(task)` always enqueues for a later drain turn, including when called by the owner.

No foreign producer executes application callbacks inline.
Queue operations are thread-safe and preserve one total FIFO order at the point of successful enqueue.

### Turn and reentrancy semantics

A pump turn swaps or extracts the work admitted before that turn and invokes it on the owner thread.
Work deferred by a running task is not invoked on that task's stack.

To preserve current executor semantics:

- owner-thread `dispatch` may run inline when not closed;
- foreign-thread `dispatch` always queues;
- every `defer` queues; and
- tasks queued during a drain run in a subsequent logical turn, even if the pump continues before returning to its caller.

The exact batch implementation may drain repeated batches, but it exposes a turn boundary for tests and prevents recursive `defer` execution.
One throwing task does not corrupt or permanently wedge the queue.
Root exception handling follows the async diagnostic contract rather than silently abandoning queued work.

### Pump contract

Expose headless-host operations such as:

```text
runOneTurn()
runUntil(predicate or task completion, stop token/deadline)
runUntilIdle()
closeAdmission()
drainAndClose() / cancelAndClose()
```

Only the owner thread may pump.
Foreign threads can enqueue and wake it but cannot steal drain ownership.

`runUntil` waits on the queue condition and pumps until the awaited runtime operation reaches a terminal state, cancellation is requested, or the host deadline/policy ends.
It does not busy-spin or sleep-poll.

`runUntilIdle` is appropriate only after all producers relevant to the operation have quiesced; an empty queue alone is not proof that worker work will not enqueue later.

### CLI composition

`CliRuntime` owns one `HeadlessExecutor` before constructing `CoreRuntime`.
CLI command execution occurs on its owner thread.

Synchronous library reads/writes can continue directly.
Commands that start asynchronous runtime work receive a task/future/terminal handle and call the headless pump until that handle completes.
Completion, library change publication, reporting feeds, and diagnostics then run on the same owner thread before the command encodes its final output.

The command adapter cannot block the owner on a primitive whose completion requires an unpumped callback.
Contributor guidance and tests make this rule explicit.

At invocation end, CLI:

1. closes command-level request admission;
2. cancels or awaits owned operations;
3. lets `CoreRuntime` stop and join worker producers;
4. drains or discards already admitted callback work according to typed ownership; and
5. closes the executor before releasing callback targets.

The exact order must align with `CoreRuntime` ownership so no worker can enqueue into a destroyed executor.

### Shutdown and admission

Executor state distinguishes `Open`, `Closing`, and `Closed`.
Once closing begins, new unowned work is rejected or routed to a diagnostic policy; tasks already admitted are either drained on the owner or explicitly cancelled/discarded by their owning handle.

The executor itself does not guess whether a callback is lossless state, a cancellable observation, or a required terminal completion.
Runtime operation owners close their producers and settle handles before executor destruction.

Destruction asserts or fail-fasts if producer admission remains open or queued required work has not been resolved.
It never runs arbitrary callbacks on the destroying foreign thread.

### Test executors

Keep a narrowly named single-thread inline executor only for pure unit tests whose contract cannot receive foreign producers.
Its type/documentation must not claim the shared thread-safe executor semantics, and it cannot be injected into `async::Runtime` tests that exercise worker resumption.

Prefer deterministic queued/headless executors in runtime, Player, reporting, and lifetime tests so their turn and affinity behavior matches production.

## Alternatives

### Protect `ImmediateExecutor` with a mutex

A mutex can protect the queue but cannot make foreign-thread inline `dispatch` execute on one owner thread.
Changing it into the proposed queued/pumped type is clearer than retaining a misleading name.

### Give headless runtime a dedicated callback thread

A dedicated thread naturally drains work, but every synchronous CLI runtime call would need marshaling to that thread and a result handoff.
An owner-thread pump preserves the current command-thread model while making waits explicit.

### Use the Boost.Asio worker pool as the callback executor

The pool is multi-threaded and would not provide one serialized application-control domain without a separate strand and host lifetime protocol.
It would also blur worker versus callback ownership.

### Keep CLI synchronous forever

Runtime already contains worker-to-callback paths, and shared features will continue to use them.
Forbidding asynchronous reuse would split CLI behavior from other frontends and still leave tests with the false executor contract.

### Add locks to every runtime service

That changes the architecture from executor confinement to pervasive synchronization, expands deadlock/reentrancy risk, and does not define callback ordering.

## Compatibility and migration

CLI syntax, structured output, plain output, and exit codes do not change.
Observable ordering may become more deterministic where worker completions previously ran inline on producer threads.

`CliRuntime` changes executor type and gains pump integration for asynchronous commands.
Focused tests using `ImmediateExecutor` migrate according to their purpose: pure single-thread tests use the explicitly limited inline test helper, while concurrency/lifetime tests use `HeadlessExecutor` or `QueuedExecutor`.

Runtime APIs that expose asynchronous operations may need a terminal handle suitable for `runUntil`.
This is an execution-contract migration, not a second CLI task framework.

## Validation

- `isCurrent()` is true only on the attached owner thread.
- Many concurrent producers can call `dispatch`/`defer` without races, corruption, loss, or concurrent callback execution.
- Every callback records the same owner thread even when submitted by worker threads.
- FIFO and later-turn behavior holds for nested dispatch/defer, tasks queued during drain, and reentrant runtime publication.
- A throwing task reports once, does not wedge the executor, and leaves remaining task policy deterministic.
- `runUntil` wakes without polling and completes worker-to-callback coroutines without deadlock.
- Cancellation races settle one terminal handle and leave no callback after its target teardown.
- Close/drain tests cover enqueue-before-close, enqueue-racing-close, producer quiescence, pending work, and destruction misuse.
- CLI smoke tests exercise at least one worker round trip and prove stdout/stderr/exit behavior remains intact.
- Runtime library-change, notification/reporting, and fault-delivery tests assert owner-thread affinity.
- TSAN stress tests cover multi-producer queue and shutdown, followed by a full `./ao check`.

## Open questions

- Should `HeadlessExecutor` live beside `QueuedExecutorBase` or share a lower synchronized queue implementation with it?
- Does the owner attach at construction, first pump, or through an explicit `attachCurrentThread()` transition?
- Which queued callbacks are mandatory to drain versus safe to cancel after `CoreRuntime` producers join?
- Should root task exceptions be captured by the executor, the runtime diagnostic sink, or a wrapper installed by the headless host?
- Which existing tests genuinely require a pure inline executor after production CLI migration?

## Promotion plan

If accepted and implemented:

- update the [runtime execution architecture](../architecture/runtime-execution.md) with the headless owner-thread/pump domain and shutdown order;
- update the [CLI execution specification](../spec/cli/execution.md) with asynchronous command pumping and terminal completion rules;
- update executor API documentation with thread safety, turn, admission, and close semantics;
- update failure/reporting specifications if headless fault/feed delivery gains explicit drain behavior;
- update contributor concurrency/testing guidance with the production versus inline-test executor distinction; and
- record the owner-thread pump choice if the dedicated-thread alternative would be expensive to revisit.
