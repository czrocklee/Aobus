---
id: architecture.runtime-execution
---
# Runtime execution architecture

This page explains the execution domains shared by Aobus frontends and the
rules for crossing between them. Follow the owning subsystem for operation-specific
ordering:

- [Signal ordering and reentrancy](signal.md)
- [Library task scheduling and cancellation](../library/task-execution.md)
- [Realtime audio and generation fences](../playback/audio-execution.md)
- [Runtime placement and teardown](../session-lifecycle.md)
- [Asynchronous outcome ownership](../failure/outcome-channel.md)

## Execution domains

Each `CoreRuntime` takes ownership of a frontend-supplied `ao::async::Executor`
and constructs an `ao::async::Runtime` borrowing that callback executor, with a
Boost.Asio worker pool. The callback executor is the serialized application-control domain.
Runtime services keep mutable state there unless their own contract names a
different synchronized domain.

```text
frontend event-loop thread
  owns callback executor and runtime state
             ^        |
             |        v
       callback hop   worker pool
                         |
                         v
                  blocking/core work

audio/device callbacks
  -> subsystem-owned queues and threads
  -> Player callback gate
  -> callback executor before application-state mutation
```

GTK, TUI, WinUI, and AppKit adapt their native event loops. CLI uses an
owner-driven loop on the invocation thread and pumps it until a command's
terminal marker returns. A worker continuation never becomes a second owner of
CLI or interactive state.

### Callback executor

Registration, delivery, inspection, and teardown of an executor-affine service
all occur on its callback executor. `async::Signal` is unsynchronized and does
not choose an executor; its owner defines the domain. `Signal::post()` supplies
a weak-lifetime deferred hop, not permission to use its other operations from a
foreign thread. See the [signal contract](signal.md).

The GTK, TUI, CLI loop, and WinUI adapters derive from `QueuedExecutorBase`.
Foreign producers append to one mutex-protected FIFO; only the constructing
thread drains it. A drain takes a snapshot, releases the mutex, and executes
that snapshot non-reentrantly. Tasks admitted by those callbacks remain for a
later turn. Wake requests are coalesced, but a task accepted during a drain
still causes a later wake when needed.

For a non-empty deferred task, normal return means admission succeeded. A throw
means the executor retained nothing. Once admitted, wake failure is fatal: the
queue cannot safely roll back a task while concurrent producers may already
have observed the admitted state. An escaping queued callback is likewise fatal
after the executor restores its queue bookkeeping.

### Worker pool

Worker tasks operate on isolated values or thread-safe Core facilities. They do
not access widgets, executor-affine services, or frontend state. A typical
operation explicitly crosses both boundaries:

```text
callback executor: validate, capture values, start cancellable task
  -> worker: blocking/core work
  -> cancellation checkpoint
  -> callback executor: revalidate identity and owner lifetime
  -> commit state and notify observers
```

The callback return is not by itself sufficient to accept stale work. The owner
also checks the subsystem evidence that identifies the request: for example a
revision, route, resource identity, generation, or token. Playback preparation,
library writes, and resource delivery define those checks in their own
specifications.

`RequestCoalescer` shares equal-key work among separately cancellable callback
interests. It owns flight bookkeeping, not the external work or its lifetime.
An owner cancels its external lifetime scope before clearing the coalescer;
exact-flight completion tokens prevent an old completion from matching a
replacement flight with the same key.

### Dedicated subsystem threads

Audio engines, backend/device monitors, and stream decoders own threads whose
requirements do not fit the worker pool. They communicate through synchronized
queues, snapshots, and callback gates. They never become general application
workers and cannot mutate runtime or frontend state directly. Their owners stop
admission and quiesce admitted callbacks before destroying callback targets.
The detailed queue, generation, and realtime rules belong to
[audio execution](../playback/audio-execution.md).

## Cancellation and terminal failures

`Runtime::spawnCancellable` owns a stop source through its returned registration;
`LifetimeScope` extends that ownership to all work belonging to an object.
Cancellation is cooperative. Executor switches, timers, and subsystem work
provide checkpoints, but a blocking third-party or codec call need not return
immediately. A cancelled or superseded operation must still be rejected before
it can publish.

A callback-executor hop checks cancellation before dispatch and after resume.
During runtime shutdown, callback admission closes first. A queued hop then
drops its Asio handler instead of running application code; destroying that
handler unwinds the suspended frame, and shared callback state keeps the worker
executor alive until that destruction finishes.

Task ownership determines exception handling:

- `Runtime::spawn()` returns a `TaskFuture<T>` whose caller owns the original
  result or exception. Non-void transport uses an optional payload, so `T` need
  not have an invalid default state.
- Fire-and-forget, cancellable, and lifetime-bound roots consume expected
  cancellation. They finish their owned bookkeeping, then send every other
  escaping exception to the exception-aware AO fatal entry.
- Synchronous signal and queued-executor boundaries also convert an escaping
  callback exception into AO fatal handling. They never log and continue over
  a partially delivered state change.

See [outcome channels](../failure/outcome-channel.md) and the
[fatal facility](../failure/fatal.md) for the terminal ordering.

## Reentrancy and teardown

Synchronous observers run on the owner's stack. An observer may issue only the
reentrant operations its service documents. It must not destroy the emitting
owner or invoke composition-root shutdown on that stack; teardown is deferred
to a later executor turn. Services that permit reentrant mutation, such as the
notification feed, queue a later immutable update so every observer finishes
the current snapshot first.

Shutdown retires frontend admission and observers before stopping the runtime
producers they observe; the runtime graph remains alive until those producers
have joined:

1. Interactive owners stop accepting new lifecycle and frontend work, then
   retire frontend adapters, subscriptions, callback generations, and native
   targets.
2. `AppRuntime` stops playback-session scheduling and quiesces audio and device
   callback producers before shutting down its core.
3. `CoreRuntime` seals library command and publication admission while callback
   delivery is still available.
4. `Runtime::requestStop()` closes callback resumption and stops the pool;
   `join()` waits while storage-backed and notification collaborators remain
   alive.
5. The frontend performs only its explicitly permitted final executor drain,
   destroys runtime collaborators, and releases the callback executor last.
6. The registered fatal sink is removed only after worker, audio, device, and
   frontend callback producers have quiesced and before logger destruction.

`AppRuntime::shutdown()` and `CoreRuntime::shutdown()` are idempotent. A
non-returning blocking operation can extend the join even after cancellation
has made its result inadmissible. The [session lifecycle](../session-lifecycle.md)
owns final object placement and frontend-specific teardown.

## Structural rules

- One executor-affine service has one serialized mutable-state domain; adding a
  mutex does not waive that affinity.
- Values, stop tokens, and narrow thread-safe collaborators cross a worker
  boundary; references to executor-affine or frontend state do not.
- A lower-subsystem callback is observational until its runtime owner accepts it
  on the callback executor.
- Callback gates and weak control blocks prevent new entry; they do not replace
  producer quiescence or owner-last destruction.
- Cancellation is not proof of lifetime safety. Teardown closes admission,
  requests stop, joins producers, drains the permitted final turns, and only
  then releases targets.
- Tests use controlled executors, sleepers, barriers, and captured callbacks;
  elapsed time alone is not concurrency evidence.

## Source and test anchors

The shared mechanisms are declared by
[`Executor`](../../../include/ao/async/Executor.h),
[`QueuedExecutorBase`](../../../include/ao/async/QueuedExecutorBase.h),
[`Runtime`](../../../include/ao/async/Runtime.h),
[`LifetimeScope`](../../../include/ao/async/LifetimeScope.h), and
[`TaskFuture`](../../../include/ao/async/TaskFuture.h).
`CoreRuntime.cpp` and `AppRuntime.cpp` own the runtime shutdown boundaries.

Focused regression coverage includes `AsyncRuntimeTest.cpp`,
`LifetimeScopeTest.cpp`, `LoopExecutorTest.cpp`, `SignalTest.cpp`, and
`AppRuntimeTest.cpp` under `test/unit/`. Subsystem specifications identify their
own concurrency tests.
