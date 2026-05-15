# Async Runtime and Coroutine Adoption Plan

## Status

Proposed.

## Purpose

This document defines the long-term plan for introducing C++ coroutine-based
asynchronous workflows into Aobus while preserving clear frontend/runtime
boundaries. The immediate implementation target is a Boost.Asio-backed internal
async runtime hidden behind an Aobus-owned facade. The design intentionally keeps
the public application code independent of Boost.Asio, GTK, WinUI, and future
`std::execution` implementation details.

The goal is not to replace every callback or signal. The goal is to make
multi-step UI workflows readable, cancellable, cross-platform, and safe across
UI-thread and worker-thread transitions.

## Decision Summary

Aobus should introduce an `ao::async` facade with these responsibilities:

- represent coroutine tasks with a stable Aobus type;
- provide explicit UI and worker scheduling operations;
- provide root task spawning with exception logging;
- provide lifetime-bound task spawning for UI controllers/widgets;
- centralize cancellation semantics for UI workflows;
- keep Boost.Asio as an implementation detail.

Boost.Asio is the preferred initial implementation substrate because it is mature,
cross-platform, and already available through the project's Boost dependency
family. The facade should be designed so a future migration to C++26
`std::execution`/P2300 or another implementation does not require rewriting UI
and runtime business code.

## Goals

- Improve readability of asynchronous UI workflows such as import/export,
  file-picker flows, layout editor flows, and background preview refreshes.
- Make UI/worker thread hops explicit at each suspension point.
- Keep GTK-specific and future WinUI 3-specific scheduling details out of shared
  runtime services.
- Keep existing `Signal`/`Subscription` semantics for long-lived state
  broadcasting.
- Provide a single path for root coroutine exception logging.
- Provide a single path for canceling UI-owned tasks when their controller,
  widget, page, or window is destroyed.
- Make the worker execution resource owned by a runtime object with explicit
  shutdown semantics.
- Preserve testability through injectable schedulers/executors.

## Non-Goals

- Do not replace all callbacks with coroutines.
- Do not replace runtime signals that model observable state streams.
- Do not expose Boost.Asio types throughout application business code.
- Do not expose C++/WinRT async types from shared runtime code.
- Do not introduce a GTK-only coroutine abstraction as the shared model.
- Do not move audio real-time callbacks onto the async runtime.
- Do not migrate playback or library mutation internals before the async
  infrastructure has targeted tests.

## Terminology

| Term | Meaning |
| --- | --- |
| UI thread | The frontend thread that owns UI objects. For GTK this is the Glib/GTK main thread. For WinUI 3 this is the DispatcherQueue/apartment thread. |
| Worker thread | A background thread used for blocking or expensive work such as filesystem I/O, import/export, and preview computation. |
| UI scheduler | A frontend-provided adapter that can enqueue work onto the UI thread. |
| Async runtime | The object that owns worker execution resources and references the UI scheduler. |
| Root coroutine | A coroutine started from a callback/event handler and not awaited by a parent coroutine. |
| Lifetime scope | An object owned by a controller/widget/window that cancels all tasks started within that owner when it is destroyed. |

## Architectural Model

```text
app/linux-gtk or future app/winui
  Platform widgets, controllers, dialogs, and UI scheduler adapters.

app/async or app/runtime/async
  Aobus async facade: Task, Runtime, scheduling awaitables, spawn helpers,
  cancellation and lifetime scopes. Boost.Asio may appear here.

app/runtime
  Frontend-neutral services, commands, projections, and signals. Runtime code may
  use ao::async types only when the operation is truly frontend-neutral and
  asynchronous.

include/ao + lib
  Public core library. No dependency on ao::async, GTK, WinUI, or Boost.Asio
  coroutine machinery unless a future public API decision explicitly requires it.
```

Dependencies should remain directional:

```text
GTK shell   ─┐
             ├─> ao::async facade ─> Boost.Asio implementation detail
WinUI shell ─┘

GTK shell / WinUI shell ─> app/runtime ─> include/ao + lib
app/runtime may depend on ao::async only for frontend-neutral async commands.
```

## Coroutine Adoption Rule

Use coroutines for **one-shot workflows** and keep signals for **ongoing state
observation**.

Good coroutine candidates:

- open a file dialog, wait for selection, import files, update UI;
- export a playlist, write data on a worker, report completion;
- compute a smart-list preview on a worker, then update the dialog;
- save layout changes, reload the host, show errors;
- run a background query, then refresh a GTK/WinUI model.

Keep `Signal`/`Subscription` for:

- playback state changes;
- selection changes;
- track/list mutation broadcasts;
- now-playing updates;
- long-lived UI bindings from runtime state to widgets.

Coroutines may await a one-time event later, but `await_signal` should not be part
of the first implementation phase because it has subtle ownership, cancellation,
and reentrancy requirements.

## Public Facade Shape

The exact API can evolve during implementation, but application code should aim
for this shape:

```cpp
namespace ao::async
{
  template<typename T>
  class Task;

  class Runtime;
  class LifetimeScope;

  Task<void> resumeOnUi(Runtime& runtime);
  Task<void> resumeOnWorker(Runtime& runtime);

  template<typename F>
  Task<std::invoke_result_t<F&>> runOnWorker(Runtime& runtime, F&& fn);

  void spawnLogged(Runtime& runtime, Task<void> task);
  void spawnWithLifetime(Runtime& runtime, LifetimeScope& scope, Task<void> task);
}
```

Business and UI code should use the Aobus facade, not raw Boost.Asio:

```cpp
ao::async::Task<void> ImportExportCoordinator::importLibraryAsync()
{
  auto files = co_await chooseFilesAsync();
  if (files.empty())
  {
    co_return;
  }

  auto result = co_await ao::async::runOnWorker(
    _asyncRuntime,
    [this, files = std::move(files)]
    {
      return _session.mutation().importFiles(files);
    });

  co_await ao::async::resumeOnUi(_asyncRuntime);
  showImportResult(result);
}
```

The code above is illustrative. The first implementation should prefer a low-risk
workflow and avoid changing library mutation threading until the safety tests are
in place.

## Implementation Substrate

### Initial substrate: Boost.Asio

Boost.Asio should be used under `ao::async` for:

- coroutine task representation;
- root coroutine spawning;
- worker pool execution;
- continuation dispatch;
- exception propagation from awaited operations.

Boost.Asio types may appear in implementation files and narrow internal headers
inside the async subsystem. They should not become the default return type of UI
controllers or runtime services unless wrapped by `ao::async` aliases/classes.

### Future substrate: stdexec / C++26 `std::execution`

The facade should intentionally align with standard execution concepts:

- scheduler: where work runs;
- operation/task: what work runs;
- value/error/stopped completion;
- explicit execution-resource transitions;
- cancellation propagation.

However, NVIDIA/stdexec remains an experimental reference implementation. It is a
good conceptual target and a possible future implementation, but not the initial
production dependency for this project.

## Runtime Ownership

Introduce an async runtime object owned near the application runtime composition
root. It should contain:

- a reference or shared ownership handle to the UI scheduler;
- a Boost.Asio worker execution resource;
- shutdown state;
- root task spawn helpers;
- test hooks for deterministic execution where practical.

Potential shape:

```cpp
namespace ao::async
{
  class Runtime final
  {
  public:
    explicit Runtime(rt::IControlExecutor& uiExecutor);
    ~Runtime();

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    rt::IControlExecutor& uiExecutor() noexcept;

    void requestStop() noexcept;
    void join();
  };
}
```

The destructor must not leave worker tasks running after services or UI objects
they might reference have been destroyed. The exact member order in the runtime
composition root must make async shutdown happen before dependent services are
invalid.

## UI Scheduler Adapters

The current GTK code already has a control-thread abstraction with `dispatch`,
`defer`, and `isCurrent`. That concept should remain the UI scheduler source of
truth. The coroutine layer should adapt it rather than introduce a competing GTK
executor.

### GTK

GTK scheduling should build on the existing control executor semantics:

- `dispatch`: enqueue from any thread and wake the GTK main thread;
- `defer`: always run on a later idle iteration;
- `isCurrent`: detect whether the current thread is the GTK control thread.

`resumeOnUi(runtime)` should resume on the GTK control thread. If already on the
UI thread, the implementation must choose and document whether it resumes inline
or defers. The default should favor predictable async behavior over surprising
reentrancy for UI workflows.

### WinUI 3

The future WinUI shell should provide an equivalent UI scheduler adapter backed
by `DispatcherQueue` or the appropriate WinUI thread-affinity mechanism.

Shared runtime code must not expose:

- `winrt::fire_and_forget`;
- `IAsyncAction`;
- `winrt::resume_foreground`;
- WinRT apartment-specific types.

Those belong at the WinUI shell boundary only.

## Threading Rules

1. GTK or WinUI objects may be accessed only after resuming on the UI scheduler.
2. Blocking filesystem, import/export, and heavy query work must run on a worker
   scheduler unless the operation is known to be cheap and UI-safe.
3. Audio real-time threads must not use the UI async runtime.
4. Coroutine code must not rely on implicit thread affinity after a suspension
   point. Resume thread must be explicit in the code or guaranteed by the awaited
   operation's contract.
5. Runtime signals should be emitted from the control thread unless the signal's
   contract explicitly states otherwise.
6. UI-owned root tasks must be lifetime-bound or explicitly documented as safely
   detached.

## Cancellation and Lifetime

`LifetimeScope` should be the standard way for a UI owner to manage tasks it
starts:

```cpp
class ImportExportCoordinator final
{
public:
  ~ImportExportCoordinator();

private:
  ao::async::LifetimeScope _tasks;
};
```

Starting work:

```cpp
ao::async::spawnWithLifetime(_asyncRuntime, _tasks, importLibraryAsync());
```

Expected semantics:

- destroying the scope requests cancellation for all associated root tasks;
- a canceled task must not resume UI-accessing code after its owner is gone;
- cancellation is cooperative, not forceful thread termination;
- worker functions must periodically check cancellation if they can run for a
  long time;
- destruction waits or otherwise guarantees that no continuation can access the
  destroyed owner.

The implementation must be conservative: if a safe non-blocking destruction
scheme is not ready, prefer requiring explicit task completion/cancellation over
pretending lifetime safety exists.

## Exception Handling

Root tasks must not be started directly with raw implementation APIs. They must
go through an Aobus spawn helper that catches and logs exceptions.

Required behavior:

- unhandled `std::exception` from a root coroutine is logged with `APP_LOG_ERROR`;
- unknown exceptions are logged;
- expected domain errors should be handled inside the workflow and surfaced to
  the user through notifications or dialog state;
- cancellation should not be logged as an error unless it indicates a bug.

## API Boundary Rules

### Allowed in UI controllers

- `ao::async::Task<T>`;
- `ao::async::Runtime&`;
- `ao::async::LifetimeScope`;
- `co_await ao::async::resumeOnUi(...)`;
- `co_await ao::async::runOnWorker(...)`;
- platform-specific awaiters for platform-specific UI operations, such as a GTK
  file dialog awaiter in the GTK shell.

### Avoid in UI controllers

- raw `boost::asio::co_spawn`;
- raw `boost::asio::awaitable` in public controller method signatures;
- raw `boost::asio::thread_pool` access;
- platform-specific coroutine types outside the owning shell.

### Allowed in runtime services

- `ao::async::Task<T>` only for frontend-neutral async commands where async
  behavior is part of the use case;
- synchronous commands and signals remain valid and should not be converted just
  to use coroutines.

### Disallowed in core library

- GTK scheduler types;
- WinUI scheduler types;
- Aobus UI lifetime scopes;
- Boost.Asio coroutine APIs unless a separate public-core design explicitly
  approves them.

## Migration Strategy

### Phase 0: Technical spike

Create a minimal branch or internal spike that proves:

- Boost.Asio coroutine headers compile under the current C++23 toolchain;
- the current CMake/Nix environment can link any needed Boost.Asio dependencies;
- a coroutine can resume from a worker thread back onto the existing GTK control
  executor;
- root coroutine exceptions are logged;
- a basic lifetime scope can prevent UI access after owner destruction.

No business workflow migration is required in this phase.

### Phase 1: Async infrastructure

Add the smallest production-ready async subsystem:

- `ao::async::Runtime`;
- `ao::async::Task<T>` facade or alias;
- `resumeOnUi`;
- `runOnWorker`;
- `spawnLogged`;
- focused tests for thread switching and exception logging.

The initial `Task<T>` may be an alias to Boost.Asio's coroutine type if this
keeps the implementation small. If the alias leaks too much Boost.Asio syntax
into consumers, replace it with a wrapper before broad adoption.

### Phase 2: Lifetime-bound UI tasks

Add:

- `LifetimeScope`;
- `spawnWithLifetime`;
- cancellation tests;
- owner-destruction tests.

This phase should complete before migrating workflows that capture controller or
widget references across suspension points.

### Phase 3: First low-risk workflow migration

Migrate one isolated UI workflow with clear before/after behavior. Preferred
candidates:

- playlist export;
- library import/export coordinator flow;
- layout editor open/save flow;
- smart-list preview refresh.

Avoid starting with playback internals or library mutation internals because they
have wider state-machine and threading implications.

### Phase 4: Runtime async commands

After the UI workflow model is proven, introduce async variants for runtime
commands that naturally involve background work:

- `importFilesAsync`;
- `exportLibraryAsync`;
- `scanLibraryAsync`;
- `computePreviewAsync`.

Keep existing synchronous commands when they are simple, safe, and already used
by tests. Add async APIs only where they reduce real complexity.

### Phase 5: WinUI 3 adapter

When the WinUI 3 shell begins, implement a WinUI UI scheduler adapter against the
same `ao::async` contracts. Do not fork the async model for WinUI-specific
coroutine types.

## Testing Plan

### Unit tests

- `resumeOnUi` resumes on the injected UI/control executor.
- `runOnWorker` runs work off the UI thread and returns the value to the awaiting
  coroutine.
- Exceptions thrown on worker operations reach the awaiting coroutine.
- Exceptions escaping root tasks are logged and do not terminate the process.
- Cancellation prevents post-destruction UI continuation.
- Runtime shutdown stops accepting new work and joins worker resources.

### Integration tests

- Migrated UI workflow preserves user-visible behavior.
- Import/export progress and completion signals remain delivered on the control
  thread.
- Existing playback and track-list subscriptions are unaffected.

### Sanitizer/build validation

- Run the normal debug build with sanitizers after infrastructure changes.
- Run focused tests first, then broaden to the full test binary when touching
  runtime composition or shutdown.

## Risk Register

| Risk | Mitigation |
| --- | --- |
| Boost.Asio types leak into business code | Keep all spawn/scheduler APIs behind `ao::async`; review public headers. |
| UI owner destroyed while coroutine is suspended | Require `spawnWithLifetime` for UI-owned root tasks; test owner destruction. |
| Reentrancy from inline UI resume | Document and test `resumeOnUi` semantics; prefer defer for UI workflow safety unless performance requires inline resume. |
| Worker task outlives runtime services | Make async runtime shutdown order explicit in the composition root. |
| Signals become accidentally cross-thread | Keep signal emission contracts clear; dispatch back to control thread before emitting UI-observed runtime signals. |
| Audio thread accidentally uses async runtime | Keep audio real-time callbacks on the existing lock-free/dispatch model. |
| Future WinUI shell requires different primitives | Keep `ao::async` frontend-neutral; isolate WinUI-specific scheduler code in the WinUI shell. |
| Future stdexec adoption | Align concepts with schedulers/tasks/cancellation, but keep the facade stable. |

## Review Checklist for Coroutine Code

- Does the code explicitly state where it resumes after background work?
- Does it access UI objects only on the UI scheduler?
- Is every root task started through `spawnLogged` or `spawnWithLifetime`?
- If the coroutine captures `this`, is it lifetime-bound to the owner?
- Are exceptions handled at the workflow boundary?
- Is cancellation treated as expected control flow?
- Does the code avoid raw Boost.Asio APIs outside the async subsystem?
- Does the operation belong in a coroutine workflow rather than a long-lived
  signal subscription?

## Open Questions

- Should `resumeOnUi` run inline when already on the UI thread or always defer to
  a later control-loop iteration?
- Should `Runtime` be owned directly by the existing application runtime
  container or by a separate shell-level composition object?
- How much cancellation should be exposed through `std::stop_token` versus an
  Aobus-specific token facade?
- Should the first `Task<T>` be a direct Boost.Asio alias or a wrapper from day
  one?
- Which workflow should be the first production migration after infrastructure
  tests pass?

## Recommended First Implementation Target

The first production migration should be a low-risk UI workflow with visible
benefit and limited service impact. The best candidates are import/export dialog
coordination or smart-list preview refresh. Playback service internals and
library mutation service internals should remain unchanged until the async
runtime, lifetime cancellation, and shutdown behavior have been proven.
