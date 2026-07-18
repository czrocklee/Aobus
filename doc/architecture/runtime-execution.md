---
id: architecture.runtime-execution
type: architecture
status: current
domain: runtime
summary: Defines callback-executor affinity, worker execution, dedicated subsystem threads, cancellation ownership, and runtime teardown order.
---
# Runtime execution architecture

## Scope

This document owns the process-level execution model shared by Aobus frontends.
It identifies executor ownership, worker and dedicated-thread roles, callback affinity, cancellation boundaries, and teardown dependencies.

It does not define the detailed ordering of an individual library task, playback transition, or audio callback.
Those observable contracts belong in subsystem specifications.

## System context

Every runtime receives one frontend-selected `ao::async::Executor` as its callback executor.
`ao::async::Runtime` combines that executor with a Boost.Asio worker pool.
Subsystems that have hard realtime, device, or streaming requirements own additional dedicated threads below the application runtime.

```text
frontend event-loop thread
  owns callback Executor
        ^          |
        |          v
  callback state   async::Runtime worker pool
                         |
                         v
                  blocking/background work

audio/device boundary
  -> engine event worker, backend threads, decoder threads
  -> Player marshals observations to callback Executor
```

## Responsibilities

### Callback executor

The callback executor is the serialized application-control domain.
Mutable runtime services such as playback, sequence, view, workspace, and notification services keep their authoritative state there unless a public contract explicitly states otherwise.
Subscription registration, event delivery, and subscription teardown follow the owning service's executor affinity.

`async::Signal` is the reusable synchronous observer mechanism below runtime and UIModel services.
It is unsynchronized and does not choose an executor, so its owner defines the serialized domain for connection, emission, disconnection, inspection, and destruction.
Its `post()` operation is a weak-lifetime deferred hop through a supplied executor, not permission for other signal operations to cross threads.
The [signal delivery specification](../spec/async/signal.md) owns its exact ordering, reentrancy, observer-exception, and destruction behavior.

The notification service refines synchronous callback delivery with a revision queue.
One effective feed command installs an immutable snapshot and publishes one canonical update; a command invoked by an observer appends a later revision rather than nesting signal delivery.
Observer failure is contained after every connected observer has run and is reported through `Runtime::reportUnhandledException`, so it cannot unwind an already committed feed command.
For transient notification lifetime, the service schedules a cancellable worker sleep through the same runtime and defers completion to the callback executor.
Only a callback carrying the current notification id and lifetime generation may commit expiry; updates restart the duration, while cancellation merely avoids obsolete work.

GTK supplies `GtkMainContextExecutor`, which wakes and drains work through `Glib::Dispatcher` on the GTK main context.
TUI supplies its `Executor`, which posts work into the FTXUI screen loop.
CLI supplies `LoopExecutor`, which uses the invocation thread as owner and exposes explicit blocking and non-blocking turn operations.
`CliRuntime::runTask()` drives those turns until a terminal marker returns through the callback executor, so a worker continuation never becomes a second CLI state owner.

The GTK, TUI, and loop adapters share `QueuedExecutorBase`.
Producer threads admit foreign dispatches and deferred tasks into one mutex-protected FIFO, while only the constructing owner thread drains and executes it.
An owner drain is non-reentrant: it extracts the entry snapshot, releases the queue mutex, and then executes that snapshot.
Tasks admitted while it runs remain pending for a later executor turn.
The first task in a pending burst owns the wake request, and drain completion requests one follow-up wake when later work remains; this coalesces redundant event-loop notifications without losing the final wake.

### Worker pool

`ao::async::Runtime` owns a general-purpose worker pool for asynchronous application tasks.
Library scans, import/export, identity indexing, delayed checkpoints, and other potentially blocking work run there and explicitly resume on the callback executor before touching executor-affine state or returning UI-facing completion.

Boost.Asio owns coroutine exception transport and passes an escaping exception to the terminal `co_spawn` completion handler as `std::exception_ptr`.
For fire-and-forget roots, `Runtime` filters expected cancellation and forwards every other exception to an injected thread-safe diagnostic handler.
Future-returning tasks retain explicit caller ownership and are not also reported by that handler.
`Runtime::spawn` exposes that ownership as `TaskFuture<T>`.
For non-void tasks, its private standard future carries `std::optional<T>`, separating transport readiness from domain construction so result types do not need an invalid default state.
The [outcome channel specification](../spec/failure/outcome-channel.md) owns the exact terminal ordering and fallback behavior.

The worker pool is not a second application-state owner.
Worker tasks operate on thread-safe/core facilities or isolated values and publish results back through the callback boundary.

Mutating library tasks enter coordinator maintenance on the callback executor before slow preparation begins.
The maintenance guard may accompany worker preparation, but it carries no LMDB transaction and no writer mutex.
Worker code acquires a coordinator-owned mutation only for a bounded apply/commit phase; the committed revision is then dispatched back through the callback executor and synchronously reduced before maintenance exit can advertise authoring availability.
Read-only export and scan-plan construction need no maintenance admission.

### Dedicated subsystem threads

The audio subsystem owns threads whose lifetime and scheduling requirements do not fit the general worker pool.
These include engine event delivery, backend render/device-monitor work, and per-stream decoding.
They communicate through synchronized queues, snapshots, and callbacks rather than accessing frontend or runtime state directly.

The logging backend may also own its own asynchronous worker, but it is infrastructure rather than an application-control domain.

## Boundaries and dependency direction

- Frontends construct the callback executor and transfer exclusive ownership to `CoreRuntime`.
- `CoreRuntime` owns `async::Runtime`; runtime services borrow it or its callback executor and cannot outlive it.
- Interactive composition injects an async exception handler from the application logging boundary; `ao_async` does not depend on application logging types.
- Runtime and UIModel event owners may use `async::Signal`, but application payloads, affinity checks, transaction ordering, and observer-failure containment remain with those owners.
- Worker tasks may resume on the callback executor through `Runtime::resumeOnCallbackExecutor`.
- Runtime library code cannot bypass `LibraryMutationService` with an independent committing transaction; UIModel and frontend code cannot name that authority.
- A synchronous non-toolkit adapter that starts such a task drives its owner loop rather than blocking on a future whose completion may require that loop.
- Notification feed reads, commands, and subscription registration require the callback executor; foreign producers return through their runtime owner instead of using a cross-thread convenience post.
- Frontend code does not post directly into audio engine internals, and audio callbacks do not mutate runtime snapshots from backend threads.
- UIModel and frontend adapters call executor-affine runtime services only from the owning event-loop thread.
- Dedicated subsystem threads remain implementation details below the runtime service that translates them into application state.

## Data and control flow

An asynchronous application operation uses an explicit round trip:

```text
callback executor
  -> start cancellable coroutine
  -> worker pool performs blocking/core work
  -> resume on callback executor
  -> update runtime state and notify observers
```

A mutating library task refines the round trip:

```text
callback executor: enter Maintenance(operationKind)
  -> worker pool: parse, walk, hash, or otherwise prepare without writer ownership
  -> bounded coordinator mutation: revalidate, apply, commit revision R
  -> callback executor: publish LibraryChangeSet R and run synchronous reducers
  -> exit maintenance and publish Available(runtimeInstanceId, R)
```

Cancellation before commit releases maintenance without advancing the library revision.
After a transaction may have committed, the coroutine returns to the callback executor without a cancellable hop so publication and maintenance cleanup cannot be skipped.

For CLI, the callback executor is the invocation thread's `LoopExecutor` and the synchronous command boundary pumps it through `CliRuntime::runTask()` until terminal completion.
If a callback throws while pumping, CLI retains the first exception and continues to the terminal marker before consuming the spawned future; command-owned task inputs therefore cannot unwind while worker work still uses them.
A task failure remains the primary command exception and the retained callback failure is reported; otherwise the retained callback failure is rethrown on the invocation thread.

An audio observation uses a separate bridge:

```text
backend/decoder callback
  -> Engine event queue
  -> Player callback gate
  -> callback executor
  -> PlaybackService snapshot/event
  -> UIModel/frontend observer
```

The callback executor is therefore the convergence point for application-visible state even when the work originates on several independent threads.
The current Engine non-realtime queue and Player-to-executor task stream do not form one bounded/coalescing observation boundary; [RFC 0028](../rfc/0028-bounded-audio-observation-delivery.md) proposes that refinement.

## Structural constraints

- An executor-affine service owns one serialized mutable state domain; adding a mutex is not a substitute for respecting that domain.
- Background work carries values, stop tokens, and narrow thread-safe collaborators across the boundary, not references to frontend widgets or executor-affine view state.
- A maintenance guard closes interactive admission across slow preparation but never grants storage write access by itself.
- A callback from a lower subsystem is observational until it has been marshalled to the owning executor and accepted by the runtime service.
- Synchronous observer delivery cannot destroy the emitting owner on the same callback stack; teardown is deferred to a later executor turn.
- Reentrant notification publication is revision-queued, so observers of revision R retain one immutable snapshot even if an earlier observer commits revision R+1.
- Notification expiry tasks never mutate feed state on a worker; a stale, cancelled, or owner-retired expiry callback is rejected on the callback executor.
- A dedicated audio or device thread cannot become a general application worker.
- Tests replace time, execution, or backend facilities through explicit executor and sleeper seams instead of relying on sleeps.

## Failure, cancellation, and lifetime boundaries

`Runtime::spawnCancellable` owns a stop source through its returned scoped registration.
Higher-level owners retain that handle or use a lifetime scope so cancellation is requested when the operation or owner ends.
Cancellation is cooperative and checked at executor switches, timers, and subsystem-specific checkpoints.

`spawnLogged`, `spawnCancellable`, and lifetime-bound completion use the same injected exception handler after their terminal ownership bookkeeping.
The handler may run concurrently on worker threads and therefore must synchronize its own mutable state.
A frontend workflow that catches an unexpected exception before a cancellable callback hop reports it immediately, then performs only generic presentation on the callback executor; later cancellation may suppress presentation but cannot erase the diagnostic.

Runtime shutdown proceeds from producers toward dependencies:

1. Interactive runtime owners stop playback-session scheduling and quiesce audio callback producers.
2. Frontend subscriptions and adapters release their observations.
3. `CoreRuntime` requests worker-pool stop and joins it while storage-backed and notification collaborators still exist.
4. Library, source, completion, and notification collaborators are destroyed.
5. The callback executor is released last within `CoreRuntime` ownership.

The application logger and its captured async-exception adapter outlive step 3 and are shut down only after worker completion handlers have quiesced.
CLI follows the same producer-first order, then drains already-ready loop turns while `CoreRuntime` callback targets remain alive before releasing that runtime and executor.

Dedicated audio and device owners request stop and join their own threads inside their shutdown or destruction boundary.
Unexpected coroutine exceptions are reported by the async runtime; expected cancellation is not reported as an unhandled failure.

## Implementation map

- [`ao::async::Executor`](../../include/ao/async/Executor.h) defines callback dispatch and deferred-turn semantics.
- [`ao::async::Signal`](../../include/ao/async/Signal.h) and [`ao::async::Subscription`](../../include/ao/async/Subscription.h) define owner-affine observer delivery and scoped connection lifetime.
- [`QueuedExecutorBase`](../../include/ao/async/QueuedExecutorBase.h) implements the multi-producer, owner-drained FIFO and wake-coalescing turn boundary used by GTK, TUI, and explicit loops.
- [`LoopExecutor`](../../include/ao/async/LoopExecutor.h) adds the binary wake signal and owner-driven blocking/non-blocking turn operations.
- [`ao::async::Runtime`](../../include/ao/async/Runtime.h) owns the worker pool and coroutine switching operations.
- [`TaskFuture`](../../include/ao/async/TaskFuture.h) owns explicit future result and exception transport without default-constructing domain values.
- [`AsyncExceptionHandler`](../../include/ao/async/AsyncExceptionHandler.h) is the injected terminal diagnostic seam.
- [`Runtime.cpp`](../../lib/async/Runtime.cpp) implements worker spawning, cancellation, timers, and callback resumption.
- [`CoreRuntime.cpp`](../../app/runtime/CoreRuntime.cpp) owns executor/runtime lifetime and worker shutdown ordering.
- [`NotificationService.cpp`](../../app/runtime/NotificationService.cpp) enforces reporting-feed affinity and deterministic reentrant publication on that executor.
- [`Log.cpp`](../../app/runtime/Log.cpp) adapts terminal exceptions to the retained application logger.
- [`AppRuntime.cpp`](../../app/runtime/AppRuntime.cpp) orders playback-session and player shutdown ahead of base-runtime teardown.
- [`GtkMainContextExecutor`](../../app/linux-gtk/app/GtkMainContextExecutor.cpp), [`tui::Executor`](../../app/tui/Executor.cpp), and [`CliRuntime`](../../app/cli/CliRuntime.cpp) adapt the three frontend execution models.
- [`Engine.cpp`](../../lib/audio/Engine.cpp) and [`StreamingSource.cpp`](../../lib/audio/StreamingSource.cpp) contain the principal dedicated audio-thread boundaries.

## Test map

- [`AsyncRuntimeTest.cpp`](../../test/unit/runtime/AsyncRuntimeTest.cpp) tests executor switching, cancellation, terminal exception ownership, non-default-constructible result transport, and runtime lifetime.
- [`LifetimeScopeTest.cpp`](../../test/unit/runtime/LifetimeScopeTest.cpp) tests lifetime bookkeeping and injected exception delivery.
- [`LoopExecutorTest.cpp`](../../test/unit/runtime/LoopExecutorTest.cpp) protects owner affinity, burst wake coalescing, multi-producer admission, non-reentrant turns, and later-turn delivery.
- [`SignalTest.cpp`](../../test/unit/async/SignalTest.cpp) protects connection order, reentrant mutation, nested emission, observer failures, deferred turns, and weak owner lifetime independently of application runtime composition.
- [`CliRuntimeTest.cpp`](../../test/unit/cli/CliRuntimeTest.cpp) protects CLI worker round trips, callback-failure task completion, terminal exception propagation, and producer-first callback draining.
- [`EngineConcurrencyTest.cpp`](../../test/unit/audio/EngineConcurrencyTest.cpp) protects the audio control/event thread boundary.
- [`EngineCallbackTest.cpp`](../../test/unit/audio/EngineCallbackTest.cpp) protects callback delivery and teardown constraints.
- [`PlayerTest.cpp`](../../test/unit/audio/PlayerTest.cpp) protects marshalling from engine/provider events to the callback executor.
- [`PlaybackServiceTest.cpp`](../../test/unit/runtime/PlaybackServiceTest.cpp) and [`PlaybackSequenceServiceTest.cpp`](../../test/unit/runtime/PlaybackSequenceServiceTest.cpp) exercise executor-affine application services.
- [`NotificationServiceTest.cpp`](../../test/unit/runtime/NotificationServiceTest.cpp) exercises immutable revision delivery, reentrant commands, and observer-fault containment.
- [`NotificationServiceExpiryTest.cpp`](../../test/unit/runtime/NotificationServiceExpiryTest.cpp) exercises sleeper injection, deferred expiry, generation rejection, cancellation races, and queued-callback teardown.

## Related documents

- [System architecture](system-overview.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Outcome channel specification](../spec/failure/outcome-channel.md)
- [Signal delivery specification](../spec/async/signal.md)
- [Notification feed specification](../spec/reporting/notification-feed.md)
- [Library architecture](library.md)
- [Playback architecture](playback.md)
- [Workspace architecture](workspace.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Audio execution and concurrency specification](../spec/playback/audio-execution.md)
- [Concurrency and sanitizer guidance](../development/test/concurrency-and-sanitizer.md) for contributor validation workflow
- [RFC 0027: loop executor for non-toolkit hosts](../rfc/0027-loop-executor.md)
- [RFC 0028: bounded audio observation delivery](../rfc/0028-bounded-audio-observation-delivery.md)
