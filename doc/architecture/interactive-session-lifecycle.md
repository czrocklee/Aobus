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
       owns CoreRuntime + workspace/views + playback + session persistence
  -> UIModel and frontend observers
  -> checkpoint or frontend-specific library transition
  -> observers destroyed before AppRuntime
  -> callback producers stopped before runtime dependencies
```

There is no frontend-neutral stateful lifecycle service. GTK and WinUI share
the `ao_desktop_launch` value/mechanism boundary for root identity, pure
startup/switch plans, the private successor protocol, and Boost.Process-based
detached creation. Each still keeps one library-bound graph per desktop process
and opens a different root by destroying that graph before launching a
successor. Application registration, queued-work admission, state stores,
checkpoint presentation, graph release, native activation, and process exit
remain frontend-owned.
TUI creates one runtime for the selected root and does not run either desktop transition sequence.

## Responsibilities

### Interactive runtime composition

`AppRuntime` owns one `CoreRuntime` and adds `ViewService`, `WorkspaceService`, playback transport and succession, the workspace `ConfigStore`, and playback-session persistence.
It forwards the audited application-facing core services without exposing the core owner or raw storage access.
It is the lifetime root for these services rather than a universal behavioral facade.
`AppRuntime::create()` requires an owning workspace `ConfigStore` and returns `InvalidInput` for its absence before building the internal service graph.
It first completes `CoreRuntime` storage validation and initial All Tracks materialization, then constructs interactive services, and exposes ownership only after both stages succeed.
An omitted playback-session store uses that workspace store; an explicit playback-session store remains a separate override.

`CoreRuntime` remains the smaller composition used by CLI workflows and owns no interactive session lifecycle.
Its public construction is also a `Result<std::unique_ptr<...>>` factory rather than a throwing constructor.

### GTK composition root

GTK owns one main-window/runtime pair for the active library and never constructs a second pair in that process.
Application-global configuration, shell layout stores, component state, application preferences, library database, per-library workspace state, views, sources, playback stack, runtime observers, and the window all finish their owned teardown before a successor is launched.

`MainWindow` is the one GTK session owner. It sequences per-library presentation-preference loading, library-backed page initialization, workspace restoration, default-view creation, playback restoration, and checkpoints, and it separates preparation from activation: preparation builds library-backed views and shell layout without restoring playback or starting process-wide adapters, while activation selects ordinary startup restore or successor idle-start behavior and starts MPRIS.
Ordinary restore admits playback observation immediately; successor idle start keeps playback observation pending until the selected root is durable.
Workspace restoration retains the exact presentation stored with every restored view.
GTK resolves a per-list preference or recommendation and submits it as new-view-default intent; `WorkspaceService` decides whether navigation reuses a plain view or creates one.
`app/linux-gtk/main.cpp` owns the destructive handoff because it must return from the picker callback, terminally retire playback persistence, unwind the complete GTK graph, and only then launch the successor.
The shared successor parser owns the paired private marker/root request;
`GtkStartupPlan` partitions the remaining Aobus-owned options from GTK
passthrough arguments before application construction.
Every primary permits GApplication replacement, while a valid private successor requests replacement programmatically; the standard `--gapplication-replace` option remains in the GTK-owned argument partition.
`SuccessorProcessLauncher` selects the GTK executable, environment, activation,
and inherited-standard-stream policy around the shared Boost.Process V2
launcher.

### TUI composition root

TUI constructs one `AppRuntime` for the command-line-selected root and retains it for the terminal process lifetime.
It opens an initial All Tracks view through `LibraryController`.
Its output overlay uses the shared UIModel output-device selector, but TUI does
not currently persist that selection around its event loop.
It currently does not restore and checkpoint workspace or playback sessions around its event loop, so GTK lifecycle behavior must not be described as frontend-neutral current policy.

### WinUI composition root

WinUI `App` owns the dispatcher and one `LibraryWindowSession`.
That owner contains one `MainWindow` and one `LibrarySession`; the session owns application-global settings and exactly one `std::unique_ptr<rt::AppRuntime>` from which library reads, playback, resources, commands, and activity state derive.
Neither the window owner nor the session can replace or retarget its runtime.

Opening a different root posts a restart request to the application dispatcher.
After the picker coroutine returns, `App` checkpoints the old window graph and
terminally retires its global playback payload. Retirement failure leaves the
old graph running. Success releases the session, runtime, and application-state
stores, launches the exact current executable through the shared detached
launcher with the paired private request, and exits.
The successor process validates and opens that explicit root as its only graph.
No parent and successor `LibrarySession` overlap on the supported restart path, and WinUI does not retain old-library playback across the process boundary.

After platform audio providers are registered, every WinUI startup asks the
shared pure UIModel policy to resolve the persisted preferred output selection,
then submits any result before UI controllers bind. This global route preference
is independent of the library-bound playback-session admission gate. Ordinary startup then starts
playback-session observation and restores listening intent. An explicit
successor root remains a pending desktop-setting value during construction and
starts playback idle with persistence dormant. After native window and
process-adapter activation, the successor saves a desktop-settings candidate,
including the already loaded preferred output selection. Success installs the candidate
and starts playback observation; failure retains the prior live settings root
and permanently seals playback writes. Selecting an output row immediately
updates the in-memory exact requested preference; the next ordinary settings
checkpoint persists it without replacing it from the runtime snapshot. It starts an initial scan when the
carried intent requests one or the canonical database did not already exist.
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
  -> shared pure planner selects strict successor, existing durable root, or empty fallback
  -> GTK creates the fallback directory when selected
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

### GTK destructive restart

```text
validated root request
  -> defer until the native picker callback returns
  -> checkpoint and terminally retire playback-session persistence
  -> store an in-memory restart request and quit the GTK main loop
  -> close callback admission and release window, MPRIS, runtime, workers, stores, style, and GtkApplication
  -> remove process signal sources
  -> create and detach Boost.Process(exact executable, paired private successor/root arguments)
  -> exit the parent
  -> successor requests GApplication replacement and strictly constructs and activates its only pair with idle playback
  -> record the new root in global application state best-effort
       success -> admit playback observation and playback checkpoints
       failure -> keep the prior root, seal playback writes, and exclude root/playback from later window checkpoints
  -> optionally scan the selected root
```

The current implementation reuses the runtime for the same normalized root and performs the sequence above for a different root.
It neither retargets a live `MusicLibrary` nor prepares the target in the parent.
Terminal retirement physically deletes the restorable playback group and permanently seals the old process against queued, delayed, explicit, hide-time, or destruction-time playback saves.
The parent does not persist the request.
The successor treats its explicit root strictly, starts playback Idle, and commits the selected path only after activation.
Only commit success admits playback observation and playback checkpoints.
Commit failure does not roll back the usable successor: it permanently seals playback writes and excludes the selected root and playback from later window checkpoints, while window geometry, output selection, column layout, and workspace saves remain available.
All original parent graph teardown precedes the process-launch call, so the supported parent-spawned transition never overlaps that parent and its successor graphs.

Every GTK application registers with `ALLOW_REPLACEMENT`.
A valid paired `--aobus-successor` request also selects `REPLACE`, so the child can take the fixed application ID even if the D-Bus daemon still observes the old parent's connection.
The standard `--gapplication-replace` option is parsed by GTK, not Aobus, and can independently request the same name takeover without selecting successor startup behavior.
An ordinary invocation with neither mechanism remains remote only while a primary owns the name.
There is no broker or continuous registration lease between `runApp()` and successor registration: an independently launched invocation can become primary in that interval, and the successor may begin its graph before that displaced instance completes asynchronous name-lost teardown.
GApplication replacement therefore removes the old-parent name-release ordering precondition but does not prove serialization against independently launched processes.
The [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md) owns exact current transitions and failure outcomes.

### WinUI destructive restart

```text
folder-picker completion
  -> shared switch planner validates identity and returns an absolute-root request
  -> App queues that request once
  -> picker coroutine returns
  -> checkpoint MainWindow and terminally retire playback persistence
       failure -> report against the live window and return to Running
  -> release MainWindow
  -> release LibrarySession, AppRuntime, settings stores, and playback store
  -> shared detached launcher(exact executable, paired private successor request)
  -> parent exits
  -> successor shared planner validates and constructs its only session with idle playback
  -> register providers, resolve persisted output intent through UIModel, and submit any result
  -> activate native window and process adapters
  -> save a selected-root settings candidate
       success -> install candidate and start playback observation
       failure -> retain prior root and seal playback writes
  -> optionally scan the selected root
```

The old process performs no target-library validation beyond path normalization and same-root detection.
Target directory, database, writer-lease, runtime, and window failures therefore belong to successor startup.
There is no readiness handshake or rollback protocol.
Process creation policy is shared, while executable discovery, diagnostics, and
exit remain parent-owned: failure is reported after teardown and the parent
still exits. The native default passes no inheritable handle list.
[Decision 0005](../decision/0005-use-process-restart-for-winui-library-switching.md) records the accepted tradeoff.

### Shutdown

GTK requests a final checkpoint, closes callback admission, removes the active window, and releases frontend controllers, widgets, platform adapters, and subscriptions before the associated runtime.
`AppRuntime::shutdown()` then shuts down playback-session scheduling and audio callback producers before shutting down its owned Core boundary.
`CoreRuntime::shutdown()` seals library mutation and publication admission before callback resumption closes, then stops and joins asynchronous workers while library-backed collaborators still exist.
Both boundaries are idempotent so explicit composition-root shutdown and destructor fallback preserve the same order.

TUI exits its event loop, stops playback intent, calls the AppRuntime shutdown boundary, and releases its single composition without the GTK checkpoint and restart protocol.

WinUI closes the window, detaches session and native-media callbacks, releases XAML controllers, then destroys `LibrarySession`.
The session invalidates the active scan's guarded presentation closure, requests task stop, and releases its single runtime while stores and dispatcher still exist.
A destructive restart in either desktop frontend uses its ordinary shutdown direction before process creation.

## Structural constraints

- One interactive runtime is bound to one music root and database path for its complete lifetime.
- A desktop library transition replaces every library-bound runtime service and observer through a successor process; TUI has no transition command.
- Application-global and per-library managed state have distinct lifetimes.
- Frontend observers and callbacks cannot outlive the runtime services they address.
- Runtime callback producers quiesce before their targets are destroyed.
- On each supported parent-spawned restart path, GTK and WinUI do not construct the successor library graph until that original parent graph and its configuration writers are gone.
- Both desktop frontends admit a successor's global playback writer only after
  the matching root is durable; commit failure leaves the prior root and no
  playback payload.
- Shared root/protocol/launch rules do not imply a common teardown proof. GTK,
  WinUI, and TUI lifecycle asymmetry remains explicit rather than hidden behind
  a stateful common abstraction.
- Workspace, playback, persistence, presentation, and runtime execution retain ownership of their internal state and behavior.

## Failure, cancellation, and lifetime boundaries

GTK aborts destructive restart when terminal retirement cannot remove the old restorable playback session.
The old window presents the retirement error in a parent-bound message and remains the active, usable pair; no restart request is committed and no process is launched.
Several current checkpoint paths remain best-effort or log-only, so successful terminal retirement is not proof that every old payload became durable.
The grouped store now makes each requested mutation a fail-closed one-shot replacement, but it does not add workflow acknowledgement.
There is no generic transaction receipt or recovery state machine.

GTK defers retirement until after the portal callback returns so a dialog callback does not synchronously destroy its own window and coordinator.
A retired old window cannot recreate the deleted playback payload or overwrite a target selection during hide or destruction.
Before a native Open Library completion can request a switch, it must enter the callback scope owned by its `ImportExportCoordinator`.
Destroying the pair closes that coordinator; a completion delivered afterward cannot enter the closed scope or reach the old pair.
Native cancellation is requested during teardown but is not the lifetime proof.
Application shutdown closes the outer callback scope and cancels its single pending idle registration before saving and releasing the active pair.

After terminal retirement succeeds, process-launch and target-startup failures do not reconstruct the old graph.
Launch failure is diagnosed by a fresh non-unique GTK application after the old graph and signal sources are gone.
Target validation, database, runtime, and activation failures are diagnosed by the successor after its failed composition has unwound.
The prior durable root remains unchanged because only an activated successor records the request.
If that post-activation root commit fails, GTK keeps the successor usable but permanently seals its playback writes and excludes root/playback from later window checkpoints.
Natural playback, explicit playback save, hide, destruction, and shutdown cannot create a payload associated with the prior root, while ordinary window, output, layout, and workspace saves continue.
An available desktop activation token is completed as failed when process creation fails.

GApplication replacement is a same-user desktop coordination mechanism, not a security boundary.
During the name-free interval after the original parent unregisters, an independently launched ordinary process can become primary before the intended successor registers.
The successor then replaces it, but GApplication name transfer does not wait for the displaced process to finish its ordinary quit and graph teardown; a deliberate external replacement has the same limitation.
Such overlap is outside the supported private restart protocol and is not a supported multi-library mode.

WinUI accepts a restart request only once and dispatches destructive work after the picker callback returns.
Terminal playback-retirement failure returns the process to `Running`, retains
the old window/session, and launches nothing. The parent never persists the
requested root.
After teardown, process-launch failure is reported without rollback; target open and activation failures are successor startup outcomes and leave the prior durable root unchanged.
The successor restores no playback before its root commit. Commit failure
preserves the prior in-memory settings snapshot, permanently seals playback
writes, and leaves the target runtime usable for non-playback persistence.
After successor activation, initial-scan or explicit-rescan planning and application failures are presented against that active session and do not resurrect the previous process.
What a finished scan is reported as - its verdict, severity, retention, and sentence - is decided once in UIModel and enumerated by the [library scan report reference](../reference/shell/library-scan-report.md); a session posts that decision rather than reaching its own.
An Open Library request may cancel an active scan through ordinary parent teardown; explicit Rescan still has no public cancellation or supersession command.
The dispatcher executor is the only route by which runtime callbacks may update XAML.
The window retires generation controllers and projections and destroys SMTC and artwork consumers before releasing its session; releasing that session destroys its unique `AppRuntime`, whose interactive implementation owns and destroys the shared resource-byte memory cache before the composed `CoreRuntime`.
The runtime destructor joins its worker tasks; no deferred runtime release or quarantine owner is used.

## Implementation map

- [`AppRuntime`](../../app/include/ao/rt/AppRuntime.h) and [`AppRuntime.cpp`](../../app/runtime/AppRuntime.cpp) own interactive composition and playback-first teardown.
- [`app/include/ao/desktop/`](../../app/include/ao/desktop/) and
  [`app/desktop/`](../../app/desktop/) own shared root identity, pure startup and
  switch plans, the private successor protocol, and detached process creation.
- [`GtkStartupPlan.cpp`](../../app/linux-gtk/app/GtkStartupPlan.cpp), [`LibraryWindowLifecycle.cpp`](../../app/linux-gtk/app/LibraryWindowLifecycle.cpp), [`MainWindow.cpp`](../../app/linux-gtk/app/MainWindow.cpp), [`SuccessorProcessLauncher.cpp`](../../app/linux-gtk/platform/SuccessorProcessLauncher.cpp), and [`app/linux-gtk/main.cpp`](../../app/linux-gtk/main.cpp) own GTK startup planning, prepare/activate composition, terminal retirement, complete unwind, direct process launch, diagnostics, and pair lifetime.
- [`ImportExportCoordinator`](../../app/linux-gtk/portal/ImportExportCoordinator.h) and [`MainContextCallbackScope`](../../app/linux-gtk/common/MainContextCallbackScope.h) own the guarded native chooser handoff into that lifecycle.
- [`app/tui/App.cpp`](../../app/tui/App.cpp) and [`LibraryController.cpp`](../../app/tui/LibraryController.cpp) own the current TUI process composition.
- [`OutputDeviceViewModel`](../../app/include/ao/uimodel/playback/output/OutputDeviceViewModel.h)
  and [`OutputSelection`](../../app/include/ao/uimodel/playback/output/OutputSelection.h)
  own the shared GTK, TUI, and WinUI selector projection, exact requested-intent
  callback, and pure restore policy.
- [`DesktopOutputSelection`](../../app/windows-winui/include/ao/winui/app/DesktopOutputSelection.h)
  adapts that pure policy to the Windows desktop settings value without owning IO.
- [`App.xaml.cpp`](../../app/windows-winui/App.xaml.cpp), [`LibraryWindowSession.cpp`](../../app/windows-winui/app/LibraryWindowSession.cpp), [`LibrarySession.cpp`](../../app/windows-winui/app/LibrarySession.cpp), [`ProcessLauncher.cpp`](../../app/windows-winui/platform/ProcessLauncher.cpp), and [`DispatcherQueueExecutor.cpp`](../../app/windows-winui/app/DispatcherQueueExecutor.cpp) own WinUI composition, destructive restart, process launch, and callback affinity.
- [`CoreRuntime`](../../app/include/ao/rt/CoreRuntime.h) owns the lower non-interactive composition and async shutdown boundary.

## Test map

- [`AppRuntimeTest.cpp`](../../test/unit/runtime/AppRuntimeTest.cpp) protects interactive composition and callback-producer teardown.
- [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) protects final checkpoints, terminal retirement failure, the stale-write guard, failed successor-root commit isolation, workspace and playback restoration, and checkpoint ordering while ordinary window, output, layout, and workspace saves continue.
- [`MainWindowSessionPresentationTest.cpp`](../../test/unit/linux-gtk/app/MainWindowSessionPresentationTest.cpp) protects presentation precedence across GTK workspace and playback restoration.
- [`GtkStartupPlanTest.cpp`](../../test/unit/linux-gtk/app/GtkStartupPlanTest.cpp) and [`SuccessorProcessLauncherTest.cpp`](../../test/unit/linux-gtk/platform/SuccessorProcessLauncherTest.cpp) protect the paired private `--aobus-successor` protocol, GTK-owned standard replacement passthrough, exact launch plan, activation-environment cleanup, exec failure, and detach.
- [`GApplicationReplacementTest.cpp`](../../test/unit/linux-gtk/app/GApplicationReplacementTest.cpp) protects ordinary remote activation and live-owner replacement on an isolated session bus; it does not claim serialization against an independently launched graph.
- [`PlaybackSessionTest.cpp`](../../test/unit/runtime/PlaybackSessionTest.cpp) protects terminal playback sealing against queued and delayed activity.
- [`MainContextCallbackScopeTest.cpp`](../../test/unit/linux-gtk/common/MainContextCallbackScopeTest.cpp) protects completion invalidation and teardown ordering.
- [`ImportExportCoordinatorTest.cpp`](../../test/unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp) protects native chooser policy and handoff.
- [`HeadlessShellTest.cpp`](../../test/unit/runtime/HeadlessShellTest.cpp) protects frontend-neutral reconstruction primitives without asserting a common lifecycle owner.
- [`LibraryControllerTest.cpp`](../../test/unit/tui/LibraryControllerTest.cpp) protects the current TUI composition path.
- [`OutputSelectionTest.cpp`](../../test/unit/uimodel/playback/output/OutputSelectionTest.cpp)
  protects catalog-aware admission and pure preferred/fallback resolution.
- [`DesktopOutputSelectionTest.cpp`](../../test/unit/winui/app/DesktopOutputSelectionTest.cpp)
  protects Windows startup resolution and in-memory preference updates before the next checkpoint.
- Tests under [`test/unit/desktop/`](../../test/unit/desktop/) run on Linux and
  Windows and protect shared startup, switch, protocol, argv, detach, and handle
  inheritance behavior.
- WinUI app-policy tests under [`test/unit/winui/app/`](../../test/unit/winui/app/)
  protect output-preference lifecycle, transactional explicit-root commit, and
  destructive preparation/restart order; bounded-cache tests and native WinUI
  builds protect native composition.

## Related documents

- [Architecture landscape](README.md)
- [System architecture](system-overview.md)
- [Workspace architecture](workspace.md)
- [Runtime execution architecture](runtime-execution.md)
- [Playback architecture](playback.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Presentation architecture](presentation.md)
- [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md)
- [Desktop library lifecycle specification](../spec/application/desktop-library-lifecycle.md)
- [Desktop successor protocol reference](../reference/application/desktop-successor-protocol.md)
- [Windows desktop shell specification](../spec/shell/windows-desktop.md)
- [Workspace session specification](../spec/workspace/session.md)
