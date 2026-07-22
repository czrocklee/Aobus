---
id: architecture.interactive-session-lifecycle
type: architecture
status: current
domain: application
summary: Defines construction, restoration, replacement, checkpointing, and teardown of a library-bound interactive runtime graph.
---
# Interactive session lifecycle architecture

## Scope

This document owns how GTK and TUI construct, retain, restore, checkpoint, replace, and destroy a library-bound `AppRuntime` graph.
It defines composition-root responsibilities, global versus per-library state lifetimes, observer ordering, and current frontend differences.

It does not own workspace state semantics, playback succession, managed-state schemas, presentation policy, or exact GTK transitions.
Those facts belong to workspace, playback, persistence, presentation, specifications, and reference.

## System context

The [architecture landscape](README.md) classifies interactive session lifecycle as an application system.
The [system architecture](system-overview.md) makes GTK and TUI composition roots and places `AppRuntime` in application runtime.

```text
GTK or TUI composition root
  -> callback executor + paths + stores + audio providers
  -> AppRuntime
       CoreRuntime + workspace/views + playback + session persistence
  -> UIModel and frontend observers
  -> checkpoint or active-library replacement
  -> observers destroyed before AppRuntime
  -> callback producers stopped before runtime dependencies
```

There is no current frontend-neutral lifecycle service.
GTK coordinates a restorable, replaceable window/runtime pair, while TUI creates one runtime for the selected root and does not run the GTK restoration and checkpoint sequence.

## Responsibilities

### Interactive runtime composition

`AppRuntime` extends `CoreRuntime` with `ViewService`, `WorkspaceService`, playback transport and succession, the workspace `ConfigStore`, and playback-session persistence.
It is the lifetime root for these services rather than a universal behavioral facade.
Construction requires an owning workspace `ConfigStore` and rejects its absence before building the internal service graph.
An omitted playback-session store uses that workspace store; an explicit playback-session store remains a separate override.

`CoreRuntime` remains the smaller composition used by CLI workflows and owns no interactive session lifecycle.

### GTK composition root

GTK owns one replaceable main-window/runtime pair for the active library.
Application-global configuration, shell layout stores, component state, and application preferences survive a library replacement.
The library database, per-library workspace state, views, sources, playback stack, runtime observers, and window are replaced together.

`MainWindowCoordinator` sequences per-library presentation-preference loading, library-backed page initialization, workspace restoration, default-view creation, playback restoration, and checkpoints.
Workspace restoration retains the exact presentation stored with every restored view.
GTK resolves a per-list preference or recommendation and submits it as new-view-default intent; `WorkspaceService` decides whether navigation reuses a plain view or creates one.
`app/linux-gtk/main.cpp` owns active-library replacement because the operation destroys and recreates the window-owned runtime graph.

### TUI composition root

TUI constructs one `AppRuntime` for the command-line-selected root and retains it for the terminal process lifetime.
It opens an initial All Tracks view through `LibraryController`.
It currently does not restore and checkpoint workspace or playback sessions around its event loop, so GTK lifecycle behavior must not be described as frontend-neutral current policy.

## Boundaries and dependency direction

- Frontends construct `AppRuntime`; application runtime never depends on UIModel, GTK, TUI, platform paths, or toolkit lifecycle types.
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
  -> load per-library presentation preferences
  -> initialize library-backed pages and subscriptions
  -> restore workspace views with their exact presentation state
  -> create a default All Tracks view with its resolved default when restoration is empty
  -> restore playback against the active library
  -> load shell layout and present the window
```

Workspace restoration precedes playback reveal so playback intent can be associated with a valid runtime view.
Playback restoration submits the resolved list default as new-view-default intent, so workspace reuse preserves an existing unfiltered view while creation applies that default.
Filtered views over the same list remain distinct and do not prevent creation of a plain playback-reveal target.
The behavior details remain in the workspace, playback, and GTK lifecycle specifications.

### GTK active-library replacement

```text
validated root request
  -> checkpoint old frontend and runtime state
  -> discard the old restorable playback payload
  -> mark the old window prepared against stale later saves
  -> destroy old observers, window, and AppRuntime
  -> record the new root in global application state
  -> construct and initialize a new pair
  -> optionally scan the selected root
```

The current implementation reuses the runtime for the same normalized root and replaces the complete pair for a different root.
It does not retarget a live `MusicLibrary` in place.
The [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md) owns exact current transitions and failure outcomes.

### Shutdown

GTK requests a final checkpoint, removes the active window, and releases frontend controllers, widgets, platform adapters, and subscriptions before the associated runtime.
`AppRuntime` then shuts down playback-session scheduling and audio callback producers before workspace, view, and Core dependencies are released.
`CoreRuntime` stops and joins asynchronous workers while library-backed collaborators still exist.

TUI exits its event loop, stops runtime work, and releases its single composition without the GTK checkpoint and replacement protocol.

## Structural constraints

- One interactive runtime is bound to one music root and database path for its complete lifetime.
- Replacing the active library replaces every library-bound runtime service and observer.
- Application-global and per-library managed state have distinct lifetimes.
- Frontend observers and callbacks cannot outlive the runtime services they address.
- Runtime callback producers quiesce before their targets are destroyed.
- Current GTK and TUI lifecycle asymmetry is explicit and cannot be hidden behind a proposed common abstraction.
- Workspace, playback, persistence, presentation, and runtime execution retain ownership of their internal state and behavior.

## Failure, cancellation, and lifetime boundaries

GTK aborts active-library replacement when preparation cannot discard the old restorable playback session.
The old window presents the preparation error in a parent-bound message and remains the active, usable pair; the failure also returns to the replacement caller so it cannot proceed.
Several current checkpoint paths remain best-effort or log-only, so successful preparation is not proof that every old payload became durable.
The grouped store now makes each requested mutation a fail-closed one-shot replacement, but it does not add workflow acknowledgement.
There is no generic transaction receipt or recovery state machine.

GTK defers replacement until after the portal callback returns so a dialog callback does not synchronously destroy its own window and coordinator.
A prepared old window cannot later overwrite the new global selection during hide or destruction.
Before a native Open Library completion can request replacement, it must enter the callback scope owned by its `ImportExportCoordinator`.
Replacing the pair destroys that coordinator; a completion delivered afterward cannot enter the closed scope or reach the old pair.
Native cancellation is requested during teardown but is not the lifetime proof.

[RFC 0019](../rfc/0019-safe-active-library-replacement.md) proposes preparing a replacement GTK pair before releasing the working pair.
That proposal is not current behavior; GTK and TUI lifecycle ownership remains separate.

## Implementation map

- [`AppRuntime`](../../app/include/ao/rt/AppRuntime.h) and [`AppRuntime.cpp`](../../app/runtime/AppRuntime.cpp) own interactive composition and playback-first teardown.
- [`MainWindow.cpp`](../../app/linux-gtk/app/MainWindow.cpp), [`MainWindowCoordinator.cpp`](../../app/linux-gtk/app/MainWindowCoordinator.cpp), and [`app/linux-gtk/main.cpp`](../../app/linux-gtk/main.cpp) own GTK startup, checkpoint, replacement, and pair lifetime.
- [`ImportExportCoordinator`](../../app/linux-gtk/portal/ImportExportCoordinator.h) and [`MainContextCallbackScope`](../../app/linux-gtk/common/MainContextCallbackScope.h) own the guarded native chooser handoff into that lifecycle.
- [`app/tui/App.cpp`](../../app/tui/App.cpp) and [`LibraryController.cpp`](../../app/tui/LibraryController.cpp) own the current TUI process composition.
- [`CoreRuntime`](../../app/include/ao/rt/CoreRuntime.h) owns the lower non-interactive composition and async shutdown boundary.

## Test map

- [`AppRuntimeTest.cpp`](../../test/unit/runtime/AppRuntimeTest.cpp) protects interactive composition and callback-producer teardown.
- [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) protects final checkpoints and the stale-write guard.
- [`MainWindowCoordinatorTest.cpp`](../../test/unit/linux-gtk/app/MainWindowCoordinatorTest.cpp) protects GTK restoration and checkpoint ordering.
- [`MainWindowSessionPresentationTest.cpp`](../../test/unit/linux-gtk/app/MainWindowSessionPresentationTest.cpp) protects presentation precedence across GTK workspace and playback restoration.
- [`MainContextCallbackScopeTest.cpp`](../../test/unit/linux-gtk/common/MainContextCallbackScopeTest.cpp) protects completion invalidation and teardown ordering.
- [`ImportExportCoordinatorTest.cpp`](../../test/unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp) protects native chooser policy and handoff.
- [`HeadlessShellTest.cpp`](../../test/unit/runtime/HeadlessShellTest.cpp) protects frontend-neutral reconstruction primitives without asserting a common lifecycle owner.
- [`LibraryControllerTest.cpp`](../../test/unit/tui/LibraryControllerTest.cpp) protects the current TUI composition path.

## Related documents

- [Architecture landscape](README.md)
- [System architecture](system-overview.md)
- [Workspace architecture](workspace.md)
- [Runtime execution architecture](runtime-execution.md)
- [Playback architecture](playback.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Presentation architecture](presentation.md)
- [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md)
- [Workspace session specification](../spec/workspace/session.md)
- [RFC 0019: safe active-library replacement](../rfc/0019-safe-active-library-replacement.md)
