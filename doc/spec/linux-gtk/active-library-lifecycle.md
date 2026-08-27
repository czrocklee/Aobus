---
id: linux-gtk.active-library-lifecycle
type: spec
status: current
domain: linux-gtk
summary: Defines GTK startup library selection, same-root reuse, destructive successor-process switching, checkpointing, and shutdown behavior.
---
# GTK active-library lifecycle

## Scope

This specification defines the GTK-specific application and window transitions
around the [shared desktop library lifecycle](../application/desktop-library-lifecycle.md):
GApplication registration, runtime/window composition, optional bootstrap
scanning, activation-token handoff, hide, quit, replacement, and handled process
signals.

It does not redefine shared root identity, startup planning, successor argument
grammar, detached launch, workspace payload fields, playback-session contents,
scan semantics, file-dialog rendering, or runtime-internal teardown. Those
facts belong to their application, workspace, playback, library, presentation,
persistence, and runtime-execution owners.

## Code boundary

This is a **GTK frontend composition-root** contract under the
[system architecture](../../architecture/system-overview.md) and
[interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md).
Its implementation is `app/linux-gtk/main.cpp`, `GtkStartupPlan.cpp`,
`LibraryWindowLifecycle.cpp`, `MainWindow.cpp`, and
`SuccessorProcessLauncher.cpp`. Common root, protocol, startup, and detached
process rules are supplied by `ao_app_desktop` under `app/include/ao/desktop/`
and `app/desktop/`.

GTK may select platform paths, construct stores and executors, register audio
providers, own windows, coordinate lifecycle commands, and launch its own
successor. It cannot redefine runtime workspace, playback, library, or
persistence payload semantics.

## Terminology

- **Active library** is the music root bound to the current main-window
  `AppRuntime`.
- **Window/runtime pair** is one `MainWindow` and the `AppRuntime` whose lifetime
  is attached to it.
- **Successor startup** is a private, explicit-root invocation that may replace
  the fixed GApplication registration and starts playback Idle.
- **Restart request** is parent-local state containing the normalized target
  root, scan intent, and optional desktop activation token.
- **Terminal retirement** is the old pair's checkpoint, physical playback-group
  removal, and permanent playback-persistence seal for that process.
- **Playback-persistence admission** is the successor window's
  `AwaitingRootCommit`, `Ready`, or `Sealed` gate over playback observation and
  whether a window checkpoint may include the selected root and playback.
- **Durable root** is the application session's persisted `lastLibraryPath`.
- **Bootstrap scan** is the optional scan requested after the successor has
  activated.
- **Empty fallback root** is the temporary `aobus-empty` directory used by an
  ordinary startup when no saved existing root can be selected.

## Invariants

- A GTK process owns at most one window/runtime pair and one library-bound
  graph.
- One pair remains bound to one music root and database path for its lifetime;
  switching never retargets a live runtime.
- The supported restart path launches no successor until the parent callback
  scope, windows, adapters, runtime, workers, stores, style runtime, and
  `Gtk::Application` have been released.
- After that release and process-signal removal, `main()` directly launches the
  successor; there is no temporary application-registration owner or readiness
  handshake.
- The parent never constructs or validates the target runtime and never
  persists the requested root.
- A successful terminal retirement permanently prevents every later playback
  checkpoint, delayed save, or queued snapshot callback in that parent from
  recreating the deleted payload.
- A retired window cannot write application or playback session state during
  hide, release, or destruction.
- A successor activates with idle playback and cannot interpret the old
  library's playback identities.
- The successor saves the selected root only after its window and runtime are
  active.
- A successor admits playback observation and playback checkpoints only after
  that root save succeeds. Failure permanently seals playback and excludes the
  selected root and playback from later window checkpoints, but does not block
  window, output, column-layout, or workspace saves.
- An explicit successor root is strict and never falls back to the durable or
  empty root.
- The private `--aobus-successor` marker and explicit root must appear together
  and are removed before GTK receives its argument vector. Scan intent is valid
  only for such a successor; the exact grammar belongs to the
  [desktop successor protocol reference](../../reference/application/desktop-successor-protocol.md).
- The standard `--gapplication-replace` option is GTK-owned passthrough syntax,
  not the private successor marker.
- An invocation with neither private successor mode nor the standard GTK
  replacement option remains a remote activation while a primary owns the
  fixed name.
- The fixed name does not guarantee serialization against independently
  launched invocations during the gap before successor registration.
- Opening the normalized active root reuses and presents the current pair.
- A native chooser completion cannot request a switch after its owning
  coordinator has been destroyed, and its callback returns before destructive
  retirement begins.
- Frontend observers and GTK objects are released before the window-owned
  runtime; callback producers stop before their targets and dependencies.

## State model

Each process retains:

- an optional active `MainWindow` reference;
- application-global `AppConfigStore`, shell-layout store, and component-state
  store for that process;
- global application session state containing the durable root and output
  identities;
- one window-local phase in `Constructed`, `Prepared`, `Active`, or `Retired`;
- one playback-persistence admission state in `AwaitingRootCommit`, `Ready`, or
  `Sealed`;
- one runtime-local music root, database, workspace store, and
  playback-session owner whose lifecycle is delegated to the
  [playback persistence specification](../playback/session-persistence.md); and
- an optional restart request that is moved out only after the GTK composition
  has completely unwound.

Construction creates the frontend graph without lifecycle writes. Preparation
restores library-backed and shell state and moves only from `Constructed` to
`Prepared`. Activation moves only from `Prepared` to `Active`, using either
ordinary startup restore or successor idle-start mode. Ordinary restore makes
window admission `Ready` and runtime playback persistence `Observing`; idle
start leaves those states `AwaitingRootCommit` and `Dormant` until the one
selected-root commit settles them as `Ready`/`Observing` or
`Sealed`/`WriteSealed`.
Retirement moves only from `Active` to `Retired` after checkpoint and successful
terminal playback retirement; repeated retirement is idempotent. An `Active`
window always admits `MainWindow::saveSession()`: `Ready` requests a full save,
while `AwaitingRootCommit` or `Sealed` excludes selected-root and playback writes
but still saves window geometry, output selection, column layout, and workspace.
A `Retired` window ignores later save requests. Invalid lifecycle transitions
violate fatal preconditions.

## Commands and transitions

### Parse and register the application

Startup planning runs before `Gtk::Application` construction. The shared parser
consumes the private protocol defined by its
[reference](../../reference/application/desktop-successor-protocol.md).
`GtkStartupPlan` then separates Aobus-owned verbosity, logging, help, and
version options from GTK passthrough arguments while preserving each
partition's original order. The standard
`--gapplication-replace` option stays in the GTK partition.

Every GTK application is created with `ALLOW_REPLACEMENT`. A valid successor is also
created with `REPLACE`; Aobus derives that flag from the private successor
marker rather than passing a replacement option string to GTK.
`REPLACE` removes any requirement for the D-Bus daemon to process the old
connection close before successor registration. It does not authorize early
launch and does not prove that the parent graph has been destroyed.

GTK remains free to interpret its standard `--gapplication-replace` option.
That option can request name takeover, but it does not establish Aobus successor
mode, supply a strict root, or select idle playback. Without either replacement
mechanism, another invocation registers as remote while a primary exists,
activates that primary, and exits without replacing it.

### Select a startup root

An ordinary startup loads global application session state and passes its
optional root plus `<temporary-directory>/aobus-empty` to the shared pure
planner. A persisted root is selected only when it is an accessible directory;
otherwise GTK creates the planner's fallback directory. It then derives the
default database path and determines whether a bootstrap scan is needed.

A successor selects only its normalized absolute explicit root. The root must
still be a directory when activation begins. A recoverable typed failure while
validating that root or constructing its database, writer, runtime, or window
is a successor startup failure; neither the durable root nor the empty root is
substituted. Unexpected exceptions and invariant violations retain their fatal
process-root handling.

### Construct, restore, and activate a pair

GTK creates a main-context executor, per-library workspace `ConfigStore`, and
`AppRuntime` with the selected root and database. It injects the process-global
playback-session store, registers platform audio providers, constructs
`MainWindow`, and attaches runtime ownership to the window.

Preparation rebuilds library pages, restores workspace, creates a default All
Tracks view when necessary, refreshes actions, and loads shell layout. It does
not restore playback, start MPRIS, join the application, present the window, or
write a lifecycle checkpoint.

GTK adds the prepared window to the application and activates it. Ordinary
startup starts playback-persistence observation and restores playback intent.
Successor `StartIdle` deliberately reads no prior payload and keeps observation
pending. MPRIS starts best effort and the window is presented. Only after
successful successor activation does GTK save its explicit root best effort.
Success starts observation so later natural playback changes persist; failure
installs a permanent no-I/O playback-write seal and keeps the prior root with no
playback payload. Window geometry, output selection, column layout, and
workspace remain eligible for ordinary saves in either pending or sealed mode.
GTK then requests the carried bootstrap scan.

### Open the active root

Open Library delegates validation, normalization, and identity comparison to
the shared switch planner. When the selected directory is the same filesystem
object as the current runtime root, including a symlink alias, GTK keeps the
existing pair, optionally requests a bootstrap scan, and presents the existing
window. No process is launched and no lifecycle state changes.

### Switch to another root

For a different valid directory, GTK performs:

1. The guarded chooser completion records the selected path and schedules one
   GLib idle callback, then returns.
2. The idle callback requests the ordinary window/session checkpoint.
3. It calls terminal playback retirement, which physically removes the global
   `playback-session` group, seals persistence, disconnects its subscriptions,
   and cancels pending save work.
4. If retirement succeeds, it obtains an optional desktop activation token,
   stores the restart request in parent-local memory, and calls
   `Gtk::Application::quit()`.
5. GTK returns from its main loop. Callback admission closes and cancels the
   idle registration before preferences and the main window are released.
6. MainWindow observers, dialogs, MPRIS, runtime audio and worker producers,
   runtime services, stores, style state, and `Gtk::Application` are destroyed
   in their owned shutdown order.
7. After the composition function returns, process signal sources are removed.
8. `main()` delegates to the shared Boost.Process V2 launcher to create and
   detach the exact executable with the encoded private successor request, then
   exits.
9. The successor derives `REPLACE` from that private mode, registers the fixed
   application ID, and follows the strict startup and activation path above.

GTK explicitly requests inherited standard streams from the shared launcher.
A valid executable `$APPIMAGE` path is preferred for AppImage packaging;
otherwise the literal `/proc/self/exe` path is used while the parent still
exists. The GTK adapter preserves the child environment except that any
inherited `XDG_ACTIVATION_TOKEN` is removed; a newly obtained non-empty token is
then supplied under that name.

Successor activation and selected-root commit run without another GTK main-loop
turn between them. The commit therefore settles playback observation and
root/playback checkpoint inclusion before user or deferred playback work can be
observed.

### Retire the old pair

Retirement is idempotent after success. Its first call saves the active window
and runtime state, then requests
`AppRuntime::retirePlaybackSessionForLibrarySwitch()`.

If physical playback-group removal fails, retirement returns that failure and
leaves the window `Active`, usable, and able to retry. Its existing persistence
admission does not change: an ordinary `Ready` window may still checkpoint,
while a successor already `Sealed` by root-commit failure remains sealed. If
removal succeeds, runtime playback persistence enters `Retired`, its
pending debounce and subscriptions are closed, restorable runtime snapshots are
discarded, and the window enters `Retired`. Later save, hide, destruction, and
runtime shutdown paths cannot recreate the payload.

### Ordinary save and shutdown

An active window requests session save on explicit save, hide, destruction, and
application release. With `Ready` admission, the coordinator captures window
geometry, per-library column and presentation state, global root/output values,
playback session, and workspace session through their respective owners. With
`AwaitingRootCommit` or `Sealed` admission, it still saves window geometry,
output selection, column layout, and workspace, but preserves the loaded durable
root and omits playback. A `Retired` window treats later save requests as
no-ops.

Application quit, external GApplication replacement, and handled `SIGINT` or
`SIGTERM` all return through the same GTK unwind. The release helper gives an
eligible active window a final save opportunity, removes it from the
application, and releases it. A sealed successor takes the restricted save path;
a retired switch parent treats that opportunity as a no-op.

## Failure and cancellation

Selecting a path that is not a directory is a no-op. Same-root scan failure and
progress belong to the library task contract.

Playback terminal-retirement failure occurs before the parent commits its
restart request. The old window presents a parent-bound diagnostic and remains
active; no child is launched. Checkpoint sub-operations that are defined as
best effort remain log-only and do not provide a cross-store transaction.

After retirement succeeds there is no old-window rollback. Process-creation
failure is parent-owned: the activation context is notified of launch failure,
a fresh `NON_UNIQUE` diagnostic application presents a native message after all
old sources and graph objects are gone, and the parent exits. Recoverable typed
target-root and composition failures are successor-owned startup failures; the
successor fully unwinds its failed composition, presents the same kind of native
diagnostic, and exits. Unexpected exceptions and invariant violations remain
fatal failures rather than entering this recovery path.

The parent does not change the durable root. A successor changes it only after
activation. Therefore launch and target-startup failures leave the previous
durable root available to a later ordinary launch. A selected-root persistence
failure after activation is logged, leaves the successor usable, and also
leaves the previous durable root on disk. It permanently seals playback writes
and excludes the selected root and playback from every later window checkpoint,
so natural playback, explicit playback save, hide, destruction, and shutdown
cannot attach a new-library payload to that root. The same window checkpoints
continue to persist window geometry, output selection, column layout, and
workspace while preserving the prior durable root. A later Open Library
transition may still retry physical playback-group removal through terminal
retirement. Initial-scan failure does not roll back an active successor.

The file-dialog callback consumes expected cancellation or dismissal. Other
native chooser failures are logged and presented against the active parent.
Every native completion first enters the scope owned by its
`ImportExportCoordinator`; teardown closes that scope before requesting native
cancellation. The destructive handoff has one idle registration, and shutdown
closes callback admission and removes that source before destroying the window.

The direct launch leaves no continuous fixed-name lease between the original
parent's unregistration and successor registration. An independently launched
ordinary invocation may become primary in that interval. The intended successor
then uses `REPLACE`, and the displaced application's default name-lost handling
quits its main loop, but D-Bus name transfer does not wait for that graph to
finish teardown. A same-user process using standard
`--gapplication-replace` has the same limitation. The original switch parent's
library composition is fully torn down before launch, but the protocol does not
guarantee serialization against these independent invocations; their overlap is
not a supported multi-library mode.

Runtime-internal worker and audio quiescence belong to the
[runtime execution](../../architecture/runtime-execution.md) and
[playback](../../architecture/playback.md) architectures.

## Persistence and versioning

Global application session state records `lastLibraryPath` independently from
the per-library workspace file. The exact fields belong to the
[application managed-state surface](../../reference/persistence/application-config.md),
and paths belong to [managed file locations](../../reference/persistence/location.md).

The playback payload remains one global group. Terminal retirement first makes
the transition `old root + old playback` → `old root + no playback`.
A successful successor commit then yields `new root + no playback` and admits
future playback saves; a failed commit retains `old root + no playback` and
permanently seals those saves. No per-root playback schema or migration is
required.
Workspace restore/save behavior belongs to the
[workspace session specification](../workspace/session.md), and playback
payload behavior belongs to playback specifications.

## Frontend observations

Successful same-root Open Library presents the existing window. Successful
different-root Open Library stops playback, closes the old window and process,
and opens a new PID and window with idle playback. Focus transfer uses desktop
activation when the compositor supplies a token.

There is no public switching progress or rollback state. After terminal
retirement, spawn or recoverable typed startup failure produces a standalone
native diagnostic and no old library window. A later ordinary launch may
reopen the previous durable root.

## Implementation map

- [`app/linux-gtk/main.cpp`](../../../app/linux-gtk/main.cpp) owns ordinary and
  successor composition, guarded switch handoff, complete unwind, selected-root
  commit, activation-token completion, and standalone startup diagnostics.
- [`ao_app_desktop`](../../../app/desktop/) owns root identity, switch/startup
  planning, the private protocol, and detached process creation.
- [`GtkStartupPlan`](../../../app/linux-gtk/app/GtkStartupPlan.h) owns
  registration mode and Aobus/GTK argument partitioning around the shared
  protocol.
- [`LibraryWindowLifecycle`](../../../app/linux-gtk/app/LibraryWindowLifecycle.h)
  owns only pair preparation and application activation.
- [`MainWindow`](../../../app/linux-gtk/app/MainWindow.h) owns lifecycle phases,
  checkpoint entry, activation mode, terminal retirement, and stale-write
  prevention.
- [`SuccessorProcessLauncher`](../../../app/linux-gtk/platform/SuccessorProcessLauncher.h)
  owns executable selection, activation, child environment, and GTK standard
  stream policy around the shared launcher.
- [`AppRuntime`](../../../app/include/ao/rt/AppRuntime.h) and
  [`PlaybackSessionPersistence`](../../../app/runtime/PlaybackSessionPersistence.h)
  own terminal playback-persistence sealing and runtime shutdown.
- [`ImportExportCoordinator`](../../../app/linux-gtk/portal/ImportExportCoordinator.h)
  owns the platform chooser entry and guarded completion.

## Test map

- Tests under [`test/unit/desktop/`](../../../test/unit/desktop/) prove shared
  root identity, startup planning, protocol round trips, and detached-launch
  behavior on both hosts.
- [`GtkStartupPlanTest.cpp`](../../../test/unit/linux-gtk/app/GtkStartupPlanTest.cpp)
  proves exact Aobus/GTK argument partitioning, registration mode, and standard
  replacement-option passthrough around the shared protocol.
- [`SuccessorProcessLauncherTest.cpp`](../../../test/unit/linux-gtk/platform/SuccessorProcessLauncherTest.cpp)
  proves executable selection, argument and token planning, child-environment
  token cleanup, exec-failure reporting, and successful detach.
- [`GApplicationReplacementTest.cpp`](../../../test/unit/linux-gtk/app/GApplicationReplacementTest.cpp)
  and its isolated probe prove that an ordinary second instance stays remote
  and a replacement becomes primary while the original owner is still live.
- [`MainWindowTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowTest.cpp)
  proves phase transitions, retirement failure recovery, terminal payload
  removal, stale root-write prevention, and the failed successor-root commit
  seal while ordinary window, output, layout, and workspace saves continue over
  the shared global store.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp)
  proves queued post-retirement playback activity, debounce, explicit
  checkpoint, repeated retirement, and shutdown cannot recreate the payload.
- [`MainContextCallbackScopeTest.cpp`](../../../test/unit/linux-gtk/common/MainContextCallbackScopeTest.cpp)
  and [`ImportExportCoordinatorTest.cpp`](../../../test/unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp)
  protect native completion invalidation and deferred handoff.

## Related documents

- [Decision 0009: use process restart for GTK library switching](../../decision/0009-use-process-restart-for-gtk-library-switching.md)
- [Desktop library lifecycle specification](../application/desktop-library-lifecycle.md)
- [Desktop successor protocol reference](../../reference/application/desktop-successor-protocol.md)
- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Workspace session](../workspace/session.md)
- [Playback architecture](../../architecture/playback.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Managed file locations](../../reference/persistence/location.md)
