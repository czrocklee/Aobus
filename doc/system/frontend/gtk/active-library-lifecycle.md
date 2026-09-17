---
id: linux-gtk.active-library-lifecycle
---
# GTK active-library lifecycle

## Scope

This specification defines GTK's native adaptation of the
[shared desktop library lifecycle](../../desktop-library-lifecycle.md):
GApplication registration, final runtime/window composition, optional bootstrap
scanning, activation-token handoff, hide, quit, replacement, and handled process
signals.

The shared page owns root identity, the restart transaction, the last
recoverable retirement gate, release-before-launch, strict successor startup,
and durable-root/playback admission. This page does not restate those rules or
own workspace, playback, scan, or persistence payload semantics.

## Code boundary

This is a **GTK frontend composition-root** contract under the
[system architecture](../../overview.md) and
[interactive session lifecycle architecture](../../session-lifecycle.md).
Its implementation is `app/linux-gtk/main.cpp`, `GtkStartupPlan.cpp`,
`LibraryWindowLifecycle.cpp`, `MainWindow.cpp`, and
`SuccessorProcessLauncher.cpp`. Common root, protocol, startup, and detached
process rules are supplied by `ao_desktop_launch` under `app/include/ao/desktop/`
and `app/desktop/`.

GTK may select platform paths, construct stores and executors, register audio
providers, own windows, coordinate lifecycle commands, and launch its own
successor. It cannot redefine runtime workspace, playback, library, or
persistence payload semantics.

## GTK state and invariants

A process owns at most one active `MainWindow`/`AppRuntime` pair. It also owns
application-global stores, one window phase (`Constructed`, `Prepared`,
`Active`, or `Retired`), one playback-admission state
(`AwaitingRootCommit`, `Ready`, or `Sealed`), and at most one restart request
moved out only after GTK composition has fully unwound.

Construction creates the frontend graph without lifecycle writes. Preparation restores library-backed and shell state and moves only from `Constructed` to
`Prepared`. Activation moves only from `Prepared` to `Active`. Ordinary activation restores
playback and enters `Ready`; successor idle activation enters
`AwaitingRootCommit` with runtime persistence `Dormant`. The selected-root
candidate commit settles those states as `Ready`/`Observing` or
`Sealed`/`WriteSealed`. Successful terminal retirement enters `Retired` and is
idempotent; invalid transitions violate fatal preconditions.
If physical playback-group removal fails, the window stays `Active` with its prior admission: `Ready` remains ready and `Sealed` remains sealed. Successful retirement cancels pending saves, disconnects playback-persistence subscriptions, and discards restorable runtime snapshots, so later lifecycle saves cannot recreate the payload.

An active `Ready` window may save all eligible session state. An active
`AwaitingRootCommit` or `Sealed` window excludes selected-root and playback
writes but may still save geometry, output selection, column layout, and
workspace. A retired window ignores later saves. These GTK gates adapt, but do
not weaken, the shared candidate-commit and permanent-write-seal contract.

A native chooser completion cannot request a switch after its owning
coordinator has been destroyed, and it must return before retirement starts.
Frontend observers and GTK objects release before the window-owned runtime;
callback producers stop before their targets and dependencies.

## Registration and argument adaptation

Startup planning runs before `Gtk::Application` construction. The shared parser
consumes the private grammar in the
[desktop successor protocol reference](../../../reference/application/desktop-successor-protocol.md).
`GtkStartupPlan` separates Aobus verbosity, logging, help, and version options
from GTK passthrough arguments while preserving each partition's order. The
standard `--gapplication-replace` option remains in the GTK partition.

Every GTK application is created with `ALLOW_REPLACEMENT`. A valid private
successor is additionally created with `REPLACE`; Aobus derives that flag from
private successor mode rather than injecting GTK option text. Private successor
mode requires its marker and explicit root together, consumes both before GTK
sees argv, and alone may carry scan intent. The standard GTK replacement option
can request name takeover, but it does not establish successor mode, provide a
strict root, or select idle playback.

An ordinary second invocation without either replacement mechanism remains a
remote activation while a primary owns the fixed application name. `REPLACE`
means the successor need not wait for D-Bus to observe the old connection's
close, but it neither authorizes early launch nor proves parent teardown.

There is no continuous fixed-name lease between parent unregistration and
successor registration. An independent invocation may become primary in that
gap and may still be tearing down when the intended successor replaces it. A
same-user process using standard `--gapplication-replace` has the same
limitation. The displaced application's default name-lost handling quits its main loop, but name transfer does not wait for graph teardown. GApplication replacement is therefore neither a serialization nor
a security boundary; this overlap is outside the supported private restart
path.

## Startup composition and activation

Ordinary startup loads the global session root and asks the shared planner to
choose it or `<temporary-directory>/aobus-empty`; GTK creates a selected empty
fallback after planning. Successor startup accepts only its strict normalized
absolute explicit root. A missing root or failure to open its database, acquire
a writer, construct the runtime, or activate a window is reported as successor
startup failure without substituting the durable or empty root.

GTK creates a main-context executor, per-library workspace store, and
`AppRuntime`, injects the process-global playback store, registers audio
providers, and only then constructs `MainWindow`. Runtime ownership is attached
to the window after its final placement.

Preparation rebuilds library pages, restores workspace, creates an All Tracks
view when needed, refreshes actions, and loads shell layout. It does not restore playback, start MPRIS, join the application, present the window, or write a lifecycle checkpoint.
GTK adds the prepared window to the application before activation. Ordinary startup starts playback observation and restores intent; successor startup reads no prior playback payload. Both start MPRIS best effort and present the window.
The successor then saves its selected-root candidate before another main-loop turn can expose user or deferred playback work. Commit success begins observation; failure is logged, permanently seals playback, and keeps root/playback out of later checkpoints while ordinary GTK and workspace state remains writable. GTK then requests any carried bootstrap scan.

## Open Library and native handoff

The shared planner validates, normalizes, and compares the selected path. A
same-filesystem-root selection, including a symlink alias, keeps and presents
the pair and may request its carried scan.

For a different root, the guarded chooser completion captures the request and
schedules one GLib idle callback, then returns. The idle turn performs the
shared checkpoint and terminal-retirement transaction. Failure remains
recoverable in the active window and is reported there before any restart request is committed. Best-effort checkpoint sub-operations remain log-only and do not form a cross-store transaction. Success obtains an optional desktop activation
token, stores the restart request, and quits the application.

Main-loop return begins GTK's release proof: callback admission closes and the
idle registration is removed before preferences and the main window are released. Window observers and dialogs, MPRIS, runtime audio and worker producers, runtime services, stores, style state, and `Gtk::Application` then unwind in their owned order. Process signal sources are then
removed. Only after the composition function has returned does `main()` launch
the exact current executable with the encoded request. There is no temporary registration owner or successor readiness handshake. See
[desktop library lifecycle](../../desktop-library-lifecycle.md) for the complete
transaction, failure boundary, and successor commit semantics.

GTK explicitly requests inherited standard streams from the shared launcher. A
valid executable `$APPIMAGE` path is preferred for AppImage packaging;
otherwise the literal `/proc/self/exe` path is used while the parent still
exists. The adapter copies the child environment except that inherited
`XDG_ACTIVATION_TOKEN` is removed; a newly obtained non-empty token is supplied
under that name.

## Save, shutdown, and cancellation

An active window saves on explicit save, hide, destruction, and application
release according to its playback-admission state. A retired window's save is a
no-op. Application quit, external GApplication replacement, and handled
`SIGINT` or `SIGTERM` use the same scoped unwind; the release helper gives an
eligible active window a final save opportunity before removing it from the
application.

Path cancellation or a non-directory selection changes nothing. Expected file
dialog dismissal is consumed. Other chooser failures are logged and presented
against the active window. Every completion enters the scope owned by
`ImportExportCoordinator`; teardown closes that scope before requesting native
cancellation. Closing admission and removing the idle source, not native
cancellation itself, prevent re-entry.

After terminal retirement there is no old-window rollback. Launch failure is
reported by a fresh `NON_UNIQUE` diagnostic application after the old graph and
sources are gone; the activation context is also notified of launch failure. Recoverable target composition failure is reported and fully
unwound by the successor. Unexpected exceptions and invariant violations remain
fatal. A later ordinary launch can reopen the previous durable root. Initial
scan failure does not roll back an active successor. A usable write-sealed successor may later request another Open Library transition, which still retries physical playback-group removal through terminal retirement.

Same-root activation presents the existing window. A different-root transition stops playback and replaces both PID and window, starting idle; focus transfer uses the compositor's activation token when available. There is no public switching-progress or rollback state.

Runtime-internal worker and audio quiescence belong to
[runtime execution](../../execution/README.md) and
[playback](../../playback/README.md).

## Persistence

Global application session state stores `lastLibraryPath` independently from
the per-library workspace. Exact fields and paths belong to the
[application managed-state surface](../../../reference/persistence/application-config.md)
and [managed file locations](../../../reference/persistence/location.md).

The global playback transition is `old root + old playback` to
`old root + no playback` during parent retirement. Successor root commit yields
`new root + no playback` and admits later playback saves; commit failure retains
`old root + no playback` and permanently seals playback writes. No per-root
playback schema or migration is implied.

## Implementation and test map

- [`main.cpp`](../../../../app/linux-gtk/main.cpp) owns composition, guarded
  handoff, complete unwind, selected-root commit, activation tokens, and
  standalone diagnostics.
- [`GtkStartupPlan`](../../../../app/linux-gtk/app/GtkStartupPlan.h) owns GTK
  registration mode and argument partitioning.
- [`LibraryWindowLifecycle`](../../../../app/linux-gtk/app/LibraryWindowLifecycle.h)
  owns pair preparation and activation; [`MainWindow`](../../../../app/linux-gtk/app/MainWindow.h)
  owns phases, checkpoint entry, retirement, and stale-write prevention.
- [`SuccessorProcessLauncher`](../../../../app/linux-gtk/platform/SuccessorProcessLauncher.h)
  owns executable, activation, environment, and standard-stream adaptation.
- [`ImportExportCoordinator`](../../../../app/linux-gtk/portal/ImportExportCoordinator.h)
  owns chooser admission; `ao_desktop_launch`, `AppRuntime`, and
  `PlaybackSessionPersistence` own the shared rules linked above.

Tests under `test/unit/desktop/` protect shared planning, identity, protocol, and
launch. `GtkStartupPlanTest.cpp`, `SuccessorProcessLauncherTest.cpp`, and
`GApplicationReplacementTest.cpp` protect GTK partitioning, executable/token
adaptation, and registration overlap. `MainWindowTest.cpp` and
`PlaybackSessionTest.cpp` protect phases, recoverable retirement, stale-write
prevention, permanent sealing, and payload non-recreation.
`MainContextCallbackScopeTest.cpp` and `ImportExportCoordinatorTest.cpp` protect
native-completion invalidation and deferred handoff. Native registration and
process behavior still require native validation.

## Related documents

- [Desktop library lifecycle](../../desktop-library-lifecycle.md)
- [Desktop successor protocol reference](../../../reference/application/desktop-successor-protocol.md)
- [Interactive session lifecycle](../../session-lifecycle.md)
- [Runtime execution](../../execution/README.md)
- [Workspace session](../../workspace/session.md)
- [Playback architecture](../../playback/README.md)
- [Persistence architecture](../../persistence/README.md)
- [Decision 0009: process restart for GTK library switching](../../../decision/0009-use-process-restart-for-gtk-library-switching.md)
