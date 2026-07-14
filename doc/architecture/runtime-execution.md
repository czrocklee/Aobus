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
Mutable runtime services such as playback, sequence, view, and workspace services keep their authoritative state there unless a public contract explicitly states otherwise.
Subscription registration, event delivery, and subscription teardown follow the owning service's executor affinity.

GTK supplies `GtkMainContextExecutor`, which wakes and drains work through `Glib::Dispatcher` on the GTK main context.
TUI supplies its `Executor`, which posts work into the FTXUI screen loop.
CLI supplies `ImmediateExecutor`, which executes dispatch inline and preserves deferred-turn ordering with a local FIFO queue.
`ImmediateExecutor` reports every calling thread as current and does not synchronize that queue, so the current CLI/test host is safe only while no foreign producer reaches it; [RFC 0027](../rfc/0027-serialized-headless-callback-executor.md) proposes a thread-safe owner-thread pump.

The GTK and TUI adapters share `QueuedExecutorBase`.
Producer threads admit foreign dispatches and deferred tasks into one mutex-protected FIFO, while only the constructing event-loop thread drains and executes it.
An owner drain is non-reentrant: it extracts the entry snapshot, releases the queue mutex, and then executes that snapshot.
Tasks admitted while it runs remain pending for a later executor turn.
The first task in a pending burst owns the wake request, and drain completion requests one follow-up wake when later work remains; this coalesces redundant event-loop notifications without losing the final wake.

### Worker pool

`ao::async::Runtime` owns a general-purpose worker pool for asynchronous application tasks.
Library scans, import/export, identity indexing, delayed checkpoints, and other potentially blocking work run there and explicitly resume on the callback executor before touching executor-affine state or returning UI-facing completion.

The worker pool is not a second application-state owner.
Worker tasks operate on thread-safe/core facilities or isolated values and publish results back through the callback boundary.

### Dedicated subsystem threads

The audio subsystem owns threads whose lifetime and scheduling requirements do not fit the general worker pool.
These include engine event delivery, backend render/device-monitor work, and per-stream decoding.
They communicate through synchronized queues, snapshots, and callbacks rather than accessing frontend or runtime state directly.

The logging backend may also own its own asynchronous worker, but it is infrastructure rather than an application-control domain.

## Boundaries and dependency direction

- Frontends construct the callback executor and transfer exclusive ownership to `CoreRuntime`.
- `CoreRuntime` owns `async::Runtime`; runtime services borrow it or its callback executor and cannot outlive it.
- Worker tasks may resume on the callback executor through `Runtime::resumeOnCallbackExecutor`.
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
- A callback from a lower subsystem is observational until it has been marshalled to the owning executor and accepted by the runtime service.
- Synchronous observer delivery cannot destroy the emitting owner on the same callback stack; teardown is deferred to a later executor turn.
- A dedicated audio or device thread cannot become a general application worker.
- Tests replace time, execution, or backend facilities through explicit executor and sleeper seams instead of relying on sleeps.

## Failure, cancellation, and lifetime boundaries

`Runtime::spawnCancellable` owns a stop source through its returned scoped registration.
Higher-level owners retain that handle or use a lifetime scope so cancellation is requested when the operation or owner ends.
Cancellation is cooperative and checked at executor switches, timers, and subsystem-specific checkpoints.

Runtime shutdown proceeds from producers toward dependencies:

1. Interactive runtime owners stop playback-session scheduling and quiesce audio callback producers.
2. Frontend subscriptions and adapters release their observations.
3. `CoreRuntime` requests worker-pool stop and joins it while storage-backed collaborators still exist.
4. Library, source, completion, and notification collaborators are destroyed.
5. The callback executor is released last within `CoreRuntime` ownership.

Dedicated audio and device owners request stop and join their own threads inside their shutdown or destruction boundary.
Unexpected coroutine exceptions are reported by the async runtime; expected cancellation is not reported as an unhandled failure.

## Implementation map

- [`ao::async::Executor`](../../include/ao/async/Executor.h) defines callback dispatch and deferred-turn semantics.
- [`QueuedExecutorBase`](../../include/ao/async/QueuedExecutor.h) implements the multi-producer, owner-drained FIFO and wake-coalescing turn boundary used by GTK and TUI.
- [`ao::async::Runtime`](../../include/ao/async/Runtime.h) owns the worker pool and coroutine switching operations.
- [`Runtime.cpp`](../../lib/async/Runtime.cpp) implements worker spawning, cancellation, timers, and callback resumption.
- [`CoreRuntime.cpp`](../../app/runtime/CoreRuntime.cpp) owns executor/runtime lifetime and worker shutdown ordering.
- [`AppRuntime.cpp`](../../app/runtime/AppRuntime.cpp) orders playback-session and player shutdown ahead of base-runtime teardown.
- [`GtkMainContextExecutor`](../../app/linux-gtk/app/GtkMainContextExecutor.cpp), [`tui::Executor`](../../app/tui/Executor.cpp), and [`ImmediateExecutor`](../../include/ao/async/ImmediateExecutor.h) adapt the three frontend execution models.
- [`Engine.cpp`](../../lib/audio/Engine.cpp) and [`StreamingSource.cpp`](../../lib/audio/StreamingSource.cpp) contain the principal dedicated audio-thread boundaries.

## Test map

- [`AsyncRuntimeTest.cpp`](../../test/unit/runtime/AsyncRuntimeTest.cpp) tests executor switching, cancellation, and runtime lifetime.
- [`QueuedExecutorTest.cpp`](../../test/unit/runtime/QueuedExecutorTest.cpp) protects burst wake coalescing, multi-producer admission, non-reentrant drains, and later-turn delivery.
- [`EngineConcurrencyTest.cpp`](../../test/unit/audio/EngineConcurrencyTest.cpp) protects the audio control/event thread boundary.
- [`EngineCallbackTest.cpp`](../../test/unit/audio/EngineCallbackTest.cpp) protects callback delivery and teardown constraints.
- [`PlayerTest.cpp`](../../test/unit/audio/PlayerTest.cpp) protects marshalling from engine/provider events to the callback executor.
- [`PlaybackServiceTest.cpp`](../../test/unit/runtime/PlaybackServiceTest.cpp) and [`PlaybackSequenceServiceTest.cpp`](../../test/unit/runtime/PlaybackSequenceServiceTest.cpp) exercise executor-affine application services.

## Related documents

- [System architecture](system-overview.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Outcome channel specification](../spec/failure/outcome-channel.md)
- [Notification feed specification](../spec/reporting/notification-feed.md)
- [Library architecture](library.md)
- [Playback architecture](playback.md)
- [Workspace architecture](workspace.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Audio execution and concurrency specification](../spec/playback/audio-execution.md)
- [Concurrency and sanitizer guidance](../development/test/concurrency-and-sanitizer.md) for contributor validation workflow
- [RFC 0026: generation-bound platform requests](../rfc/0026-generation-bound-platform-requests.md)
- [RFC 0027: serialized headless callback executor](../rfc/0027-serialized-headless-callback-executor.md)
- [RFC 0028: bounded audio observation delivery](../rfc/0028-bounded-audio-observation-delivery.md)
