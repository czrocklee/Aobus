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

Primitive choice follows delivery topology.
Notification owners broadcast already-committed state through `async::Signal`, whose owning emission boundary is `noexcept` and converts an escaping handler exception into AO fatal handling.
Library change publication instead binds one named ordinary replica callable because phase ordering requires exactly
one mandatory derived-state consumer before notifications; its owning delivery boundary catches an escape and enters
AO fatal handling before phase-two notification.
The [signal delivery specification](../spec/async/signal.md) owns signal ordering, reentrancy, handler failure, and destruction behavior.

The notification service refines synchronous callback delivery with a small publication queue.
One effective feed command installs an immutable snapshot and publishes one canonical update; a command invoked by an observer appends a later update rather than nesting signal delivery.
Feed observers are ordinary callables behind that boundary, so delivery cannot unwind an already committed feed command.
Candidate bounding and eligible history eviction complete before that commit, so rejection leaves the current snapshot and id watermark untouched while accepted eviction remains part of the same observed update.
For transient notification lifetime, the service schedules a cancellable worker sleep through the same runtime and defers completion to the callback executor.
Only a callback carrying the current notification id and lifetime generation may commit expiry; updates restart the duration, while cancellation merely avoids obsolete work.

GTK supplies `GtkMainContextExecutor`, which wakes and drains work through `Glib::Dispatcher` on the GTK main context.
TUI supplies its `Executor`, which posts work into the FTXUI screen loop.
CLI supplies `LoopExecutor`, which uses the invocation thread as owner and exposes explicit blocking and non-blocking turn operations.
WinUI supplies `DispatcherQueueExecutor`, which wakes its owner through the XAML dispatcher queue.
`CliRuntime::runTask()` drives those turns until a terminal marker returns through the callback executor, so a worker continuation never becomes a second CLI state owner.

The GTK, TUI, CLI loop, and WinUI adapters share `QueuedExecutorBase`.
Producer threads admit foreign dispatches and deferred tasks into one mutex-protected FIFO, while only the constructing owner thread drains and executes it.
Returning from `defer()` means the executor accepted the task; if admission throws, the executor retained nothing and will not execute that task.
After queue admission, event-loop wake is a `noexcept` infrastructure boundary: a wake failure is fatal rather than a recoverable rejection because concurrent producers make rollback unsafe.
An owner drain is non-reentrant: it extracts the entry snapshot, releases the queue mutex, and then executes that snapshot.
Tasks admitted while it runs remain pending for a later executor turn.
The first task in a pending burst owns the wake request, and drain completion requests one follow-up wake when later work remains; this coalesces redundant event-loop notifications without losing the final wake.

### Worker pool

`ao::async::Runtime` owns a general-purpose worker pool for asynchronous application tasks.
Library scans, import/export, identity indexing, delayed checkpoints, and other potentially blocking work run there and explicitly resume on the callback executor before touching executor-affine state or returning UI-facing completion.
Interactive resource delivery follows the same boundary without entering library maintenance: `LibraryTaskService` copies bounded immutable bytes under a worker-side read transaction, then GTK, TUI, and MPRIS perform their platform transforms or file work on workers and return through the frontend callback executor.
Those consumers carry copied values across suspension and revalidate owner lifetime plus current resource identity before publication.

Boost.Asio owns coroutine exception transport and passes an escaping exception to the terminal `co_spawn` completion handler as `std::exception_ptr`.
For fire-and-forget roots, `Runtime` filters expected cancellation and sends every other exception to the Core exception-aware fatal entry after completion bookkeeping.
Future-returning tasks retain explicit caller ownership and are not also reported by that handler.
`Runtime::spawn` exposes that ownership as `TaskFuture<T>`.
For non-void tasks, its private standard future carries `std::optional<T>`, separating transport readiness from domain construction so result types do not need an invalid default state.
The [outcome channel specification](../spec/failure/outcome-channel.md) owns the exact terminal ordering and fallback behavior.

The worker pool is not a second application-state owner.
Worker tasks operate on thread-safe/core facilities or isolated values and publish results back through the callback boundary.

`ao::async::RequestCoalescer` is an owner-executor mechanism for sharing equal-key work among independently cancellable callback interests.
It owns flight bookkeeping and exact-flight completion tokens, not the external work, cache, error policy, or executor transition.
An owner therefore cancels its external lifetime scope before clearing the coalescer; a late completion token cannot match a replacement flight for the same key.

Interactive playback uses the same rule for view-based starts and gapless lookahead.
Player captures an isolated move-only preparation value containing copied route, decoder-factory, input, and generation evidence.
An explicit-start worker first inspects the signal, then returns through a cancellation checkpoint to the callback executor so Engine can revalidate the route and synchronously obtain a non-blocking prewarm hint from the selected Backend.
The isolated value resumes on a worker to optimistically open, seek, and preroll the final decoder in that hinted mode.
A compatible gapless-lookahead worker opens the final decoder in the already-active PCM encoding, seeks, and prerolls it.
No worker opens or reconfigures a backend or accesses Player, Engine, runtime services, or frontend state; the intermediate Backend hint query occurs only after returning to the callback and Engine control domains.
After the final worker phase resumes on the callback executor, upper request/source identity and Engine playback/route context are revalidated before adoption can allocate a source generation or publish any state.
The old playback session remains authoritative during this round trip.

Mutating library tasks acquire one coordinator background-task lease on the callback executor before slow preparation begins.
The lease serializes scan, identity backfill, and YAML import preparation, but it carries no LMDB transaction or writer mutex and does not change authoring availability.
YAML import additionally enters Maintenance and closes interactive admission; scan and backfill leave it open during preparation. Their background write phases reserve writer intent before waiting, so callback-owner authoring returns unavailable instead of blocking behind the worker transaction.
Worker code acquires a coordinator-owned mutation only for its apply/commit phase; the committed revision is then dispatched back through the callback executor and synchronously reduced before task finalization.
For a foreign commit, mandatory publication `P` is admitted before the same operation admits callback-owner finalization `C`; their shared `QueuedExecutorBase` FIFO therefore runs `P` before `C`.
An owner-thread commit executes `P` inline.
These two cases make an ordinary mutating-task return a materialization barrier without a ticket, acknowledgement object, or callback-thread wait.
Identity backfill commits bounded batches, while scan apply commits its complete admitted plan at once.
Read-only export and scan-plan construction need no maintenance admission.
For progress-capable library tasks, successful cancellable callback-executor admission is the observer-side-effect boundary; the [library task execution specification](../spec/library/runtime/task-execution.md#progress-and-outcome) owns the exact conversation and terminal-pulse behavior.

### Dedicated subsystem threads

The audio subsystem owns threads whose lifetime and scheduling requirements do not fit the general worker pool.
These include engine event delivery, backend render/device-monitor work, and per-stream decoding.
They communicate through synchronized queues, snapshots, and callbacks rather than accessing frontend or runtime state directly.

The logging backend may also own its own asynchronous worker, but it is infrastructure rather than an application-control domain.
After logging initialization, the application registers one Core fatal sink backed by a non-blocking asynchronous logger path; the realtime fatal entry bypasses that sink.

## Boundaries and dependency direction

- Frontends construct the callback executor and transfer exclusive ownership to `CoreRuntime`.
- `CoreRuntime` owns `async::Runtime`; runtime services borrow it or its callback executor and cannot outlive it.
- The Core fatal backend exposes only a function-pointer registration seam; application logging depends on Core, never the reverse.
- `ao_async` invokes that Core facility directly and has no application logging callback or second terminal-reporting seam.
- Runtime and UIModel event owners may use `async::Signal`, but application payloads, affinity checks, and transaction ordering remain with those owners.
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

Playback preparation specializes that round trip:

```text
callback executor: validate request and capture isolated audio evidence
  -> worker pool: inspect the signal
  -> callback executor: revalidate route evidence and obtain the Backend prewarm hint
  -> worker pool: prepare a lossless decoder output in the hinted mode
  -> callback executor: reject stale request or ask Engine to adopt the prepared token
  -> Engine commit: open backend and compare its actual PCM mode
  -> exact match: activate the prepared source
  -> mismatch or failed optimistic preparation: synchronously rebuild the exact decoder output
  -> commit transport and succession as one settled accepted subject; a queued start failure may then recover or stop it
```

Optimistic preparation failure preserves the successful inspection so commit can retry after the backend chooses its exact mode.
Gapless lookahead also opens, seeks, and prerolls its final decoder on the worker when it can reuse an already-open compatible PCM mode; workers never open or reconfigure a backend.

A scan or identity-backfill task refines the round trip:

```text
callback executor: acquire background-task lease
  -> worker pool: parse, walk, hash, or otherwise prepare without writer ownership
  -> background mutation: revalidate, apply, commit revision R
  -> admit callback publication P(R) while writer admission remains closed
  -> callback executor: P(R) applies the replica and emits observers
  -> callback executor: later finalization C releases the lease and clears task progress
```

YAML import uses the same lease around its stronger Maintenance interval.
Cancellation before commit releases the lease or maintenance without advancing the library revision.
After a transaction may have committed, the coroutine returns to the callback executor without a cancellable hop so publication and task cleanup cannot be skipped.

For CLI, the callback executor is the invocation thread's `LoopExecutor` and the synchronous command boundary pumps it through `CliRuntime::runTask()` until terminal completion.
An escaping executor callback completes the executor's mandatory queue bookkeeping and then enters AO fatal handling at the executor boundary.
The command task itself remains caller-owned: after its terminal marker runs, `runTask()` consumes the future and rethrows that task exception on the invocation thread.

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
The current Engine non-realtime queue and Player-to-executor task stream have no combined capacity or coalescing contract.

## Structural constraints

- An executor-affine service owns one serialized mutable state domain; adding a mutex is not a substitute for respecting that domain.
- Background work carries values, stop tokens, and narrow thread-safe collaborators across the boundary, not references to frontend widgets or executor-affine view state.
- A background-task lease serializes long task preparation without closing authoring or granting storage write access; an import maintenance guard separately closes interactive admission.
- A callback from a lower subsystem is observational until it has been marshalled to the owning executor and accepted by the runtime service.
- A synchronous observer must not destroy the emitting owner or invoke composition-root shutdown on the same callback stack; it defers teardown to a later executor turn.
- Reentrant notification mutations queue another immutable update, so every contract-fulfilling observer finishes the current snapshot before delivery moves to the next one.
- Notification expiry tasks never mutate feed state on a worker; a stale, cancelled, or owner-retired expiry callback is rejected on the callback executor.
- A dedicated audio or device thread cannot become a general application worker.
- Tests replace time, execution, or backend facilities through explicit executor and sleeper seams instead of relying on sleeps.

## Failure, cancellation, and lifetime boundaries

`Runtime::spawnCancellable` owns a stop source through its returned scoped registration.
Higher-level owners retain that handle or use a lifetime scope so cancellation is requested when the operation or owner ends.
Cancellation is cooperative and checked at executor switches, timers, and subsystem-specific checkpoints.
Playback preparation has independent start and lookahead handles.
Its decoder call may not observe a stop token, so cancellation prevents executor adoption but does not promise that an in-progress file or codec call returns immediately.
The callback-resumption checkpoint is the task-level supersession fence;
application acceptance checks semantic candidate identity, and Engine separately
revalidates captured playback and route evidence.
If the callback gate remains open, an acceptance veto produces exactly one
`Conflict` completion. Cancellation, replacement, or teardown can end the task
path earlier and suppress completion.
Player closes its callback gate and cancels both handles before Engine teardown; a worker that returns after Player teardown then owns and destroys only its detached preparation value.
`CoreRuntime::shutdown()` first seals library writer/task admission and retires not-yet-running publication/finalization callbacks.
It is an outer composition boundary, not an operation permitted from a synchronous runtime observer; an observer that requests teardown defers it to a later callback-executor turn.
It then calls `Runtime::requestStop()`, which closes callback admission before stopping the worker
pool, and teardown then joins it. A callback-executor resumption queued before that boundary is
discarded instead of resuming application code; destroying its Asio handler
unwinds the suspended coroutine frame, while shared callback state keeps the
worker executor alive until that destruction completes.
Owner-calling terminal guards use a weak lifetime gate, so later destruction of
a discarded frame cannot re-enter an already-retired runtime service.
Because pool join is final rather than detached, a decoder open that does not
return can extend `CoreRuntime` shutdown even though cancellation has already
made its result inadmissible.

`spawnLogged`, `spawnCancellable`, and lifetime-bound completion do not borrow the
`Runtime` object from terminal closures.
They consume expected cancellation, finish their owned completion or scope retirement, and
invoke the exception-aware fatal entry for every other exception.
Different worker threads may reach that entry concurrently; the Core fatal backend owns
concurrent diagnostics and single-sink admission.

Runtime shutdown proceeds from producers toward dependencies:

1. Interactive runtime owners stop playback-session scheduling and quiesce audio callback producers.
2. Frontend subscriptions and adapters release their observations.
3. `CoreRuntime` seals library mutation/publication admission, retires queued library callbacks, and wakes publication waiters.
4. `CoreRuntime` closes callback resumption, requests worker-pool stop, and joins it while storage-backed and notification collaborators still exist.
5. Library, source, completion, and notification collaborators are destroyed.
6. The callback executor is released last within `CoreRuntime` ownership.

The application logger and its registered fatal sink outlive step 3 and are shut down only after worker, audio, device, and frontend callback producers have quiesced.
The application unregisters the fatal sink before destroying the logger backend.
CLI follows the same producer-first order, then drains already-ready loop turns while `CoreRuntime` callback targets remain alive before releasing that runtime and executor.

Dedicated audio and device owners request stop and join their own threads inside their shutdown or destruction boundary.
Unexpected coroutine exceptions abort through the Core fatal backend after terminal bookkeeping; expected cancellation does not enter that path.

## Implementation map

- [`ao::async::Executor`](../../include/ao/async/Executor.h) defines callback dispatch and deferred-turn semantics.
- [`ao::async::Signal`](../../include/ao/async/Signal.h) and [`ao::async::Subscription`](../../include/ao/async/Subscription.h) define owner-affine observer delivery and scoped connection lifetime.
- [`RequestCoalescer`](../../include/ao/async/RequestCoalescer.h) defines owner-affine equal-key flight sharing, per-interest cancellation, and exact-flight completion fencing.
- [`QueuedExecutorBase`](../../include/ao/async/QueuedExecutorBase.h) implements the multi-producer, owner-drained FIFO and wake-coalescing turn boundary used by GTK, TUI, and explicit loops.
- [`LoopExecutor`](../../include/ao/async/LoopExecutor.h) adds the binary wake signal and owner-driven blocking/non-blocking turn operations.
- [`ao::async::Runtime`](../../include/ao/async/Runtime.h) owns the worker pool and coroutine switching operations.
- [`TaskFuture`](../../include/ao/async/TaskFuture.h) owns explicit future result and exception transport without default-constructing domain values.
- [`Runtime.cpp`](../../lib/async/Runtime.cpp) implements worker spawning, cancellation, timers, and callback resumption.
- [`CoreRuntime.cpp`](../../app/runtime/CoreRuntime.cpp) owns executor/runtime lifetime and worker shutdown ordering.
- [`NotificationService.cpp`](../../app/runtime/NotificationService.cpp) enforces reporting-feed affinity and deterministic reentrant publication on that executor.
- [`Contract.h`](../../include/ao/Contract.h) and [`Fatal.cpp`](../../lib/utility/Fatal.cpp) define the application-independent fatal registration and abort boundary used by that adapter.
- [`AppRuntime.cpp`](../../app/runtime/AppRuntime.cpp) orders playback-session and player shutdown ahead of base-runtime teardown.
- [`GtkMainContextExecutor`](../../app/linux-gtk/app/GtkMainContextExecutor.cpp), [`tui::Executor`](../../app/tui/Executor.cpp), [`CliRuntime`](../../app/cli/CliRuntime.cpp), and [`DispatcherQueueExecutor`](../../app/windows-winui/app/DispatcherQueueExecutor.cpp) adapt the frontend execution models.
- [`Engine.cpp`](../../lib/audio/Engine.cpp) and [`StreamingSource.cpp`](../../lib/audio/StreamingSource.cpp) contain the principal dedicated audio-thread boundaries.

## Test map

- [`AsyncRuntimeTest.cpp`](../../test/unit/runtime/AsyncRuntimeTest.cpp) tests executor switching, cancellation, caller-owned exception transport, non-default-constructible results, and runtime lifetime.
- The dedicated `ao_fatal_probe` under [`test/fatal/`](../../test/fatal) and [`LogTest.cpp`](../../test/unit/runtime/LogTest.cpp) protect fatal-sink concurrency, registration, and logger lifetime independently of executor affinity.
- [`LifetimeScopeTest.cpp`](../../test/unit/runtime/LifetimeScopeTest.cpp) tests lifetime bookkeeping before cancellation or terminal fatal handling.
- [`LoopExecutorTest.cpp`](../../test/unit/runtime/LoopExecutorTest.cpp) protects owner affinity, burst wake coalescing, multi-producer admission, non-reentrant turns, and later-turn delivery.
- [`SignalTest.cpp`](../../test/unit/async/SignalTest.cpp) protects connection order, reentrant mutation, nested emission, the owning fatal boundary, deferred turns, and weak owner lifetime independently of application runtime composition.
- [`RequestCoalescerTest.cpp`](../../test/unit/async/RequestCoalescerTest.cpp) protects flight sharing, cross-thread interest cancellation, reentrant completion, clear generation fencing, and callback fanout.
- [`CliRuntimeTest.cpp`](../../test/unit/cli/CliRuntimeTest.cpp) protects CLI worker round trips, caller-owned task exception propagation, and producer-first callback draining.
- [`EngineConcurrencyTest.cpp`](../../test/unit/audio/EngineConcurrencyTest.cpp) protects the audio control/event thread boundary.
- [`EngineCallbackTest.cpp`](../../test/unit/audio/EngineCallbackTest.cpp) protects callback delivery and teardown constraints.
- [`PlayerTest.cpp`](../../test/unit/audio/PlayerTest.cpp) protects marshalling from engine/provider events to the callback executor and cancellation while optimistic preroll is blocked on a worker.
- [`PlaybackServiceTest.cpp`](../../test/unit/runtime/PlaybackServiceTest.cpp), [`PlaybackSuccessionLaunchTest.cpp`](../../test/unit/runtime/PlaybackSuccessionLaunchTest.cpp), [`PlaybackSuccessionAdvanceTest.cpp`](../../test/unit/runtime/PlaybackSuccessionAdvanceTest.cpp), and [`PlaybackSuccessionFailureTest.cpp`](../../test/unit/runtime/PlaybackSuccessionFailureTest.cpp) exercise the public playback service and executor-affine internal succession owner.
- [`NotificationServiceTest.cpp`](../../test/unit/runtime/NotificationServiceTest.cpp) exercises bounded candidate commit, keyed correlation, immutable update delivery, and reentrant commands.
- [`NotificationServiceExpiryTest.cpp`](../../test/unit/runtime/NotificationServiceExpiryTest.cpp) exercises sleeper injection, unchanged suppression, keyed lifetime transitions, deferred expiry, generation rejection, cancellation races, and queued-callback teardown.
- [`LibraryTaskServiceTest.cpp`](../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) protects task leases, interactive authoring during scan preparation, publication-before-finalization ordering, and cancellation cleanup.

## Related documents

- [System architecture](system-overview.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Outcome channel specification](../spec/failure/outcome-channel.md)
- [Fatal facility reference](../reference/failure/fatal.md)
- [Signal delivery specification](../spec/async/signal.md)
- [Notification feed specification](../spec/reporting/notification-feed.md)
- [Library architecture](library.md)
- [Playback architecture](playback.md)
- [Workspace architecture](workspace.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Audio execution and concurrency specification](../spec/playback/audio-execution.md)
- [Concurrency and sanitizer guidance](../development/test/concurrency-and-sanitizer.md) for contributor validation workflow
