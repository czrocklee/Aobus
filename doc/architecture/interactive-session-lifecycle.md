---
id: architecture.interactive-session-lifecycle
type: architecture
status: current
domain: application
summary: Defines construction, restoration, replacement, checkpointing, and teardown of a library-bound interactive runtime graph.
---
# Interactive session lifecycle architecture

## Scope

This document owns how GTK, WinUI, and TUI construct, retain, restore, checkpoint, replace, and destroy a library-bound `AppRuntime` graph.
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
  -> checkpoint or active-library replacement
  -> observers destroyed before AppRuntime
  -> callback producers stopped before runtime dependencies
```

There is no current frontend-neutral lifecycle service.
GTK coordinates a restorable, replaceable window/runtime pair. WinUI retains one window-independent `LibrarySession` with exactly one active runtime and replaces it immediately after a new runtime opens successfully. TUI creates one runtime for the selected root and does not run either desktop replacement sequence.

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

WinUI `App` owns one dispatcher executor, stable `LibrarySession`, and main window.
The session owns application-global settings and exactly one active runtime from which library reads, playback, resources, commands, and activity state derive.
Opening a different root synchronously creates and validates the replacement through `AppRuntime::create()` while the current runtime remains active.
An open failure leaves the runtime and persisted root unchanged.
On success, one non-suspending dispatcher callback checkpoints the old workspace, detaches every runtime consumer, exchanges the active runtime, rebuilds runtime-bound session services, rebinds every consumer, records the selected root best-effort, and defers old-runtime release to the next dispatcher turn.
Releasing the old runtime stops its playback; WinUI does not retain old-library playback after a switch.
An existing canonical database needs no implicit scan.
A new root becomes active first and then runs the ordinary active-runtime scan, so scan failure leaves that root active and retryable rather than rolling back.
Explicit Rescan uses the same transactional workflow and relies on `LibraryChanges` for projection updates instead of manually reloading projections.
While Open Library or Rescan is active, another request reports the current operation and starts nothing.
Each constructed runtime reloads library-backed sources before restoring its
per-library workspace. The session checkpoints that workspace with durable
desktop-state changes, before replacing its library authority, and during
teardown.
Modern/Classic switching does not participate in this lifecycle.

## Boundaries and dependency direction

- Frontends construct `AppRuntime`; application runtime never depends on UIModel, GTK, WinUI, TUI, platform paths, or toolkit lifecycle types.
- The [workspace architecture](workspace.md) owns view and aggregate semantics inside the runtime graph.
- The [playback architecture](playback.md) owns restorable listening intent and audio teardown inside the graph.
- The [persistence and managed-state architecture](persistence-and-managed-state.md) owns store, path, schema, and durable-write boundaries.
- The [runtime execution architecture](runtime-execution.md) owns callback admission, worker quiescence, cancellation, and join ordering.
- The [presentation architecture](presentation.md) owns runtime-to-UIModel-to-frontend adaptation after the runtime exists.
- Platform dialogs and portals can request lifecycle operations but do not own runtime replacement.

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

### Shutdown

GTK requests a final checkpoint, removes the active window, and releases frontend controllers, widgets, platform adapters, and subscriptions before the associated runtime.
`AppRuntime::shutdown()` then shuts down playback-session scheduling and audio callback producers before delegating to the Core boundary.
`CoreRuntime::shutdown()` seals library mutation and publication admission before callback resumption closes, then stops and joins asynchronous workers while library-backed collaborators still exist.
Both boundaries are idempotent so explicit composition-root shutdown and destructor fallback preserve the same order.

TUI exits its event loop, stops playback intent, calls the AppRuntime shutdown boundary, and releases its single composition without the GTK checkpoint and replacement protocol.

WinUI closes the window, detaches session and native-media callbacks, releases XAML controllers, then destroys `LibrarySession`. The session invalidates the active scan's guarded presentation closure, requests task stop, and releases its single runtime while stores and dispatcher still exist.

## Structural constraints

- One interactive runtime is bound to one music root and database path for its complete lifetime.
- Replacing the active library replaces every library-bound runtime service and observer.
- Application-global and per-library managed state have distinct lifetimes.
- Frontend observers and callbacks cannot outlive the runtime services they address.
- Runtime callback producers quiesce before their targets are destroyed.
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

WinUI replacement-runtime creation, open, validation, or initial-materialization failures keep the current runtime and durable selected path unchanged.
After a successful install, initial-scan or explicit-rescan planning and application failures are presented against the active runtime; an initial-scan failure does not resurrect the previous root.
WinUI exposes no public operation cancellation or supersession path.
Its dispatcher executor is the only route by which runtime callbacks may update XAML.
Old window controllers are unbound before the retiring runtime is released, and stale asynchronous artwork is rejected independently by request generation and binding lifetime.

## Implementation map

- [`AppRuntime`](../../app/include/ao/rt/AppRuntime.h) and [`AppRuntime.cpp`](../../app/runtime/AppRuntime.cpp) own interactive composition and playback-first teardown.
- [`LibraryWindowLifecycle.cpp`](../../app/linux-gtk/app/LibraryWindowLifecycle.cpp), [`MainWindow.cpp`](../../app/linux-gtk/app/MainWindow.cpp), [`MainWindowCoordinator.cpp`](../../app/linux-gtk/app/MainWindowCoordinator.cpp), and [`app/linux-gtk/main.cpp`](../../app/linux-gtk/main.cpp) own GTK prepare/activate composition, replacement ordering, checkpointing, and pair lifetime.
- [`ImportExportCoordinator`](../../app/linux-gtk/portal/ImportExportCoordinator.h) and [`MainContextCallbackScope`](../../app/linux-gtk/common/MainContextCallbackScope.h) own the guarded native chooser handoff into that lifecycle.
- [`app/tui/App.cpp`](../../app/tui/App.cpp) and [`LibraryController.cpp`](../../app/tui/LibraryController.cpp) own the current TUI process composition.
- [`App.xaml.cpp`](../../app/windows-winui/App.xaml.cpp), [`LibrarySession.cpp`](../../app/windows-winui/app/LibrarySession.cpp), and [`DispatcherQueueExecutor.cpp`](../../app/windows-winui/app/DispatcherQueueExecutor.cpp) own WinUI composition, replacement, and callback affinity.
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
- [`WindowsDesktopSettingsYamlSchemaTest.cpp`](../../test/unit/uimodel/layout/shell/WindowsDesktopSettingsYamlSchemaTest.cpp), bounded-cache tests, and native WinUI builds protect the platform-neutral and native portions of the Windows lifecycle.

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
