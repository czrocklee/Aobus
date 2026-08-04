---
id: architecture.interactive-session-lifecycle
type: architecture
status: current
domain: application
summary: Defines construction, restoration, library transition, checkpointing, and teardown of a library-bound interactive runtime graph.
---
# Interactive session lifecycle architecture

## Scope

This document owns how GTK, WinUI, and TUI construct, retain, restore, checkpoint, transition, and destroy a library-bound `AppRuntime` graph.
It defines composition-root responsibilities, global versus per-library state lifetimes, observer ordering, and current frontend differences.

It does not own workspace state semantics, playback succession, managed-state schemas, presentation policy, or exact GTK transitions.
Those facts belong to workspace, playback, persistence, presentation, specifications, and reference.

## System context

The [architecture landscape](README.md) classifies interactive session lifecycle as an application system.
The [system architecture](system-overview.md) makes GTK, WinUI, and TUI composition roots and places `AppRuntime` in application runtime.

```text
GTK, WinUI, or TUI composition root
  -> callback executor + paths + stores + audio providers
  -> AppRuntime
       CoreRuntime + workspace/views + playback + session persistence
  -> UIModel and frontend observers
  -> checkpoint or frontend-specific library transition
  -> observers destroyed before AppRuntime
  -> callback producers stopped before runtime dependencies
```

There is no current frontend-neutral lifecycle service.
GTK coordinates a restorable, replaceable window/runtime pair.
WinUI keeps exactly one `LibraryWindowSession` per process and opens a different root by destroying that graph before launching a successor process.
TUI creates one runtime for the selected root and does not run either desktop transition sequence.

## Responsibilities

### Interactive runtime composition

`AppRuntime` extends `CoreRuntime` with `ViewService`, `WorkspaceService`, playback transport and succession, the workspace `ConfigStore`, and playback-session persistence.
It is the lifetime root for these services rather than a universal behavioral facade.
`AppRuntime::create()` requires an owning workspace `ConfigStore` and returns `InvalidInput` for its absence before building the internal service graph.
It first completes `CoreRuntime` storage validation and initial All Tracks materialization, then constructs interactive services, and exposes ownership only after both stages succeed.
An omitted playback-session store uses that workspace store; an explicit playback-session store remains a separate override.

`CoreRuntime` remains the smaller composition used by CLI workflows and owns no interactive session lifecycle.
Its public construction is also a `Result<std::unique_ptr<...>>` factory rather than a throwing constructor.

### GTK composition root

GTK owns one replaceable main-window/runtime pair for the active library.
Application-global configuration, shell layout stores, component state, and application preferences survive a library replacement.
The library database, per-library workspace state, views, sources, playback stack, runtime observers, and window are replaced together.

`MainWindowCoordinator` sequences per-library presentation-preference loading, library-backed page initialization, workspace restoration, default-view creation, playback restoration, and checkpoints.
`MainWindow` separates preparation from activation: preparation builds library-backed views and shell layout without restoring playback or starting process-wide adapters, while activation selects startup restore or replacement idle-start behavior and starts MPRIS.
Workspace restoration retains the exact presentation stored with every restored view.
GTK resolves a per-list preference or recommendation and submits it as new-view-default intent; `WorkspaceService` decides whether navigation reuses a plain view or creates one.
`app/linux-gtk/main.cpp` owns active-library replacement because the operation destroys and recreates the window-owned runtime graph.

### TUI composition root

TUI constructs one `AppRuntime` for the command-line-selected root and retains it for the terminal process lifetime.
It opens an initial All Tracks view through `LibraryController`.
It currently does not restore and checkpoint workspace or playback sessions around its event loop, so GTK lifecycle behavior must not be described as frontend-neutral current policy.

### WinUI composition root

WinUI `App` owns the dispatcher and one `LibraryWindowSession`.
That owner contains one `MainWindow` and one `LibrarySession`; the session owns application-global settings and exactly one `std::unique_ptr<rt::AppRuntime>` from which library reads, playback, resources, commands, and activity state derive.
Neither the window owner nor the session can replace or retarget its runtime.

Opening a different root posts a restart request to the application dispatcher.
After the picker coroutine returns, `App` checkpoints and retires the old window graph, releases the session, runtime, and application-state stores, launches the exact current executable with the requested root, and exits.
The successor process validates and opens that explicit root as its only graph.
No parent and successor `LibrarySession` overlap on the supported restart path, and WinUI does not retain old-library playback across the process boundary.

An explicit successor root remains a pending desktop-setting value during construction.
After native window and process-adapter activation, the successor commits the selected root best effort and starts an initial scan only when the canonical database did not already exist.
Startup failure is presented by the successor and cannot reconstruct the old process; because the parent never records the request, a later ordinary launch can still select the prior durable root.
Initial-scan failure leaves the successor root active and retryable.
Explicit Rescan uses the same transactional workflow and relies on `LibraryChanges` for projection updates instead of manually reloading projections.
Modern/Classic switching remains inside one process and does not participate in this lifecycle.
Each constructed session reloads library-backed sources before restoring its per-library workspace and checkpoints that workspace with durable desktop-state changes and during teardown.

## Boundaries and dependency direction

- Frontends construct `AppRuntime`; application runtime never depends on UIModel, GTK, WinUI, TUI, platform paths, or toolkit lifecycle types.
- The [workspace architecture](workspace.md) owns view and aggregate semantics inside the runtime graph.
- The [playback architecture](playback.md) owns restorable listening intent and audio teardown inside the graph.
- The [persistence and managed-state architecture](persistence-and-managed-state.md) owns store, path, schema, and durable-write boundaries.
- The [runtime execution architecture](runtime-execution.md) owns callback admission, worker quiescence, cancellation, and join ordering.
- The [presentation architecture](presentation.md) owns runtime-to-UIModel-to-frontend adaptation after the runtime exists.
- Platform dialogs and portals can request lifecycle operations but do not own runtime replacement or process restart.

## Data and control flow

### GTK startup

```text
load global application session
  -> resolve the last existing library root or empty fallback root
  -> derive database and per-library workspace paths
  -> construct stores, providers, and AppRuntime
  -> construct MainWindow, UIModel, controllers, and adapters
  -> prepare library pages, workspace, default view, and shell layout
  -> add the window to the application
  -> activate with playback restoration and MPRIS
  -> present the window
```

Workspace restoration precedes playback reveal so playback intent can be associated with a valid runtime view.
Playback restoration submits the resolved list default as new-view-default intent, so workspace reuse preserves an existing unfiltered view while creation applies that default.
Filtered views over the same list remain distinct and do not prevent creation of a plain playback-reveal target.
The behavior details remain in the workspace, playback, and GTK lifecycle specifications.

### GTK active-library replacement

```text
validated root request
  -> prepare and configure a candidate pair while the old pair remains active
  -> checkpoint and retire the old pair, including playback-session discard
  -> activate the candidate with idle playback and replace the active slot
  -> destroy old observers, window, and AppRuntime
  -> record the new root in global application state best-effort
  -> optionally scan the selected root
```

The current implementation reuses the runtime for the same normalized root and replaces the complete pair for a different root.
It does not retarget a live `MusicLibrary` in place.
Candidate preparation does not add the window to the application, restore playback, start MPRIS, or write lifecycle checkpoints.
Runtime-factory Error or post-construction configuration failure destroys only the candidate.
The old pair becomes retired only after its checkpoint and playback-session discard succeed; after candidate activation the old frontend graph is released before its attached runtime.
Selected-path persistence occurs only after the new pair is active and the old pair has been released, and its failure does not roll back the usable in-process pair.
The [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md) owns exact current transitions and failure outcomes.

### WinUI destructive restart

```text
folder-picker completion
  -> App queues an absolute-root restart request
  -> picker coroutine returns
  -> retire and release MainWindow
  -> release LibrarySession, AppRuntime, settings stores, and playback store
  -> CreateProcessW(exact executable, --library-root <selected root>)
  -> parent exits
  -> successor validates and constructs its only session
  -> activate native window and process adapters
  -> record the selected root best effort
  -> optionally scan the selected root
```

The old process performs no target-library validation beyond path normalization and same-root detection.
Target directory, database, writer-lease, runtime, and window failures therefore belong to successor startup.
There is no readiness handshake or rollback protocol.
Process creation itself remains parent-owned: failure is reported after teardown and the parent still exits.
[Decision 0005](../decision/0005-use-process-restart-for-winui-library-switching.md) records the accepted tradeoff.

### Shutdown

GTK requests a final checkpoint, removes the active window, and releases frontend controllers, widgets, platform adapters, and subscriptions before the associated runtime.
`AppRuntime::shutdown()` then shuts down playback-session scheduling and audio callback producers before delegating to the Core boundary.
`CoreRuntime::shutdown()` seals library mutation and publication admission before callback resumption closes, then stops and joins asynchronous workers while library-backed collaborators still exist.
Both boundaries are idempotent so explicit composition-root shutdown and destructor fallback preserve the same order.

TUI exits its event loop, stops playback intent, calls the AppRuntime shutdown boundary, and releases its single composition without the GTK checkpoint and replacement protocol.

WinUI closes the window, detaches session and native-media callbacks, releases XAML controllers, then destroys `LibrarySession`.
The session invalidates the active scan's guarded presentation closure, requests task stop, and releases its single runtime while stores and dispatcher still exist.
A destructive restart uses this same shutdown direction before process creation.

## Structural constraints

- One interactive runtime is bound to one music root and database path for its complete lifetime.
- A library transition replaces every library-bound runtime service and observer, either as GTK in-process replacement or WinUI process restart.
- Application-global and per-library managed state have distinct lifetimes.
- Frontend observers and callbacks cannot outlive the runtime services they address.
- Runtime callback producers quiesce before their targets are destroyed.
- GTK prepares an in-process replacement candidate; WinUI never constructs a successor library graph until the parent graph and its configuration writers are gone.
- Current GTK, WinUI, and TUI lifecycle asymmetry is explicit and cannot be hidden behind a proposed common abstraction.
- Workspace, playback, persistence, presentation, and runtime execution retain ownership of their internal state and behavior.

## Failure, cancellation, and lifetime boundaries

GTK aborts active-library replacement when candidate preparation/configuration fails or when retirement cannot discard the old restorable playback session.
Candidate failures leave the old pair and saved selected path unchanged.
The old window presents the retirement error in a parent-bound message and remains the active, usable pair; the failure also returns to the replacement caller so it cannot proceed.
Several current checkpoint paths remain best-effort or log-only, so successful preparation is not proof that every old payload became durable.
The grouped store now makes each requested mutation a fail-closed one-shot replacement, but it does not add workflow acknowledgement.
There is no generic transaction receipt or recovery state machine.

GTK defers replacement until after the portal callback returns so a dialog callback does not synchronously destroy its own window and coordinator.
A retired old window cannot later overwrite the new global selection during hide or destruction.
Before a native Open Library completion can request replacement, it must enter the callback scope owned by its `ImportExportCoordinator`.
Replacing the pair destroys that coordinator; a completion delivered afterward cannot enter the closed scope or reach the old pair.
Native cancellation is requested during teardown but is not the lifetime proof.
Application shutdown closes the outer callback scope and cancels its single pending idle registration before saving and releasing the active pair.

WinUI accepts a restart request only once and dispatches destructive work after the picker callback returns.
The parent never persists the requested root.
After teardown, process-launch failure is reported without rollback; target open and activation failures are successor startup outcomes and leave the prior durable root unchanged.
After successor activation, initial-scan or explicit-rescan planning and application failures are presented against that active session and do not resurrect the previous process.
An Open Library request may cancel an active scan through ordinary parent teardown; explicit Rescan still has no public cancellation or supersession command.
The dispatcher executor is the only route by which runtime callbacks may update XAML.
Window controllers, projections, resource loader, SMTC bridge, and artwork tasks are unbound before the session releases its unique runtime.
The runtime destructor joins its worker tasks; no deferred runtime release or quarantine owner is used.

## Implementation map

- [`AppRuntime`](../../app/include/ao/rt/AppRuntime.h) and [`AppRuntime.cpp`](../../app/runtime/AppRuntime.cpp) own interactive composition and playback-first teardown.
- [`LibraryWindowLifecycle.cpp`](../../app/linux-gtk/app/LibraryWindowLifecycle.cpp), [`MainWindow.cpp`](../../app/linux-gtk/app/MainWindow.cpp), [`MainWindowCoordinator.cpp`](../../app/linux-gtk/app/MainWindowCoordinator.cpp), and [`app/linux-gtk/main.cpp`](../../app/linux-gtk/main.cpp) own GTK prepare/activate composition, replacement ordering, checkpointing, and pair lifetime.
- [`ImportExportCoordinator`](../../app/linux-gtk/portal/ImportExportCoordinator.h) and [`MainContextCallbackScope`](../../app/linux-gtk/common/MainContextCallbackScope.h) own the guarded native chooser handoff into that lifecycle.
- [`app/tui/App.cpp`](../../app/tui/App.cpp) and [`LibraryController.cpp`](../../app/tui/LibraryController.cpp) own the current TUI process composition.
- [`App.xaml.cpp`](../../app/windows-winui/App.xaml.cpp), [`LibraryWindowSession.cpp`](../../app/windows-winui/app/LibraryWindowSession.cpp), [`LibrarySession.cpp`](../../app/windows-winui/app/LibrarySession.cpp), [`ProcessLauncher.cpp`](../../app/windows-winui/platform/ProcessLauncher.cpp), and [`DispatcherQueueExecutor.cpp`](../../app/windows-winui/app/DispatcherQueueExecutor.cpp) own WinUI composition, destructive restart, process launch, and callback affinity.
- [`CoreRuntime`](../../app/include/ao/rt/CoreRuntime.h) owns the lower non-interactive composition and async shutdown boundary.

## Test map

- [`AppRuntimeTest.cpp`](../../test/unit/runtime/AppRuntimeTest.cpp) protects interactive composition and callback-producer teardown.
- [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) protects final checkpoints and the stale-write guard.
- [`MainWindowCoordinatorTest.cpp`](../../test/unit/linux-gtk/app/MainWindowCoordinatorTest.cpp) protects GTK restoration and checkpoint ordering.
- [`MainWindowSessionPresentationTest.cpp`](../../test/unit/linux-gtk/app/MainWindowSessionPresentationTest.cpp) protects presentation precedence across GTK workspace and playback restoration.
- [`LibraryWindowLifecycleTest.cpp`](../../test/unit/linux-gtk/app/LibraryWindowLifecycleTest.cpp) protects candidate isolation, replacement ordering, same-root reuse, persistence timing, and failure outcomes.
- [`MainContextCallbackScopeTest.cpp`](../../test/unit/linux-gtk/common/MainContextCallbackScopeTest.cpp) protects completion invalidation and teardown ordering.
- [`ImportExportCoordinatorTest.cpp`](../../test/unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp) protects native chooser policy and handoff.
- [`HeadlessShellTest.cpp`](../../test/unit/runtime/HeadlessShellTest.cpp) protects frontend-neutral reconstruction primitives without asserting a common lifecycle owner.
- [`LibraryControllerTest.cpp`](../../test/unit/tui/LibraryControllerTest.cpp) protects the current TUI composition path.
- WinUI app-policy tests under [`test/unit/winui/app/`](../../test/unit/winui/app/) protect startup arguments, explicit-root commit timing, command-line quoting, and destructive restart order; bounded-cache tests and native WinUI builds protect native composition.

## Related documents

- [Architecture landscape](README.md)
- [System architecture](system-overview.md)
- [Workspace architecture](workspace.md)
- [Runtime execution architecture](runtime-execution.md)
- [Playback architecture](playback.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Presentation architecture](presentation.md)
- [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md)
- [Windows desktop shell specification](../spec/shell/windows-desktop.md)
- [Workspace session specification](../spec/workspace/session.md)
