---
id: linux-gtk.active-library-lifecycle
type: spec
status: current
domain: linux-gtk
summary: Defines GTK startup library selection, runtime construction, same-root reuse, library replacement, checkpointing, and shutdown behavior.
---
# GTK active-library lifecycle

## Scope

This specification defines how the GTK application selects, constructs, restores, reuses, replaces, and releases its one active library runtime.
It owns the current application and window transitions around startup, Open Library, optional bootstrap scanning, hide, quit, and process-signal shutdown.

It does not define workspace payload fields, playback-session contents, library scanning behavior, file-dialog rendering, or runtime-internal teardown.
Those facts belong to the linked workspace, playback, library, presentation, and persistence owners.

## Code boundary

This is a **GTK frontend composition-root** contract under the [system architecture](../../architecture/system-overview.md) and [interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md).
Its implementation is `app/linux-gtk/main.cpp`, `LibraryWindowLifecycle.cpp`, `MainWindow.cpp`, and `MainWindowCoordinator.cpp`.

GTK may select platform paths, construct stores and executors, register audio providers, own windows, and coordinate lifecycle commands.
It cannot redefine runtime workspace, playback, library, or persistence payload semantics.

## Terminology

- **Active library** is the music root bound to the current main-window `AppRuntime`.
- **Window/runtime pair** is one `MainWindow` and the `AppRuntime` whose lifetime is attached to it.
- **Prepared candidate** is a fully constructed pair whose library, workspace, default view, and shell layout are ready but whose playback has not been restored, MPRIS has not started, and window has not joined the application.
- **Retire for switch** is the old pair's checkpoint, playback-session discard, and stale-write transition.
- **Bootstrap scan** is the optional scan requested after opening a root whose database policy requires it.
- **Empty fallback root** is the temporary `aobus-empty` directory used when no saved existing root can be opened at startup.

## Invariants

- GTK has at most one active pair; a prepared candidate may coexist outside application window membership during replacement.
- One pair remains bound to one music root and database path for its lifetime.
- Selecting a different root replaces the complete pair; it never retargets the existing runtime or storage graph.
- Application-global configuration and shell-layout stores survive replacement; per-library runtime, database, workspace store, sources, views, and playback state do not.
- Candidate preparation completes before the old pair is retired or released.
- A retired old window cannot later overwrite the new global `lastLibraryPath` during hide or destruction.
- The globally stored restorable playback session is discarded before a different library becomes active, preventing old library identities from being restored against the new root.
- Frontend observers and GTK objects are released before the window-owned runtime.
- Opening a selected root whose normalized requested path equals the current runtime root reuses the pair rather than creating a duplicate runtime.
- A native Open Library completion cannot request replacement after its owning coordinator has been destroyed.
- Candidate construction or configuration failure leaves the active pair, application membership, visibility, playback, and saved selected path unchanged.
- The selected path is saved only after candidate activation, active-slot replacement, and old-pair release.

## State model

The GTK lifecycle retains:

- an optional active `MainWindow` reference;
- application-global `AppConfigStore`, shell-layout store, and component-state store shared across pair replacement;
- global application session state containing the last selected path and output identities;
- one window-local phase in `Constructed`, `Prepared`, `Active`, or `Retired`;
- one runtime-local music root, database, workspace store, and playback-session owner.

Construction creates the frontend graph without lifecycle writes.
Preparation restores library-backed and shell state and moves only from `Constructed` to `Prepared`.
Activation moves only from `Prepared` to `Active`, using either startup restore or replacement idle-start mode.
Retirement moves only from `Active` to `Retired` after save and successful playback-session discard; repeated retirement is idempotent.
Only `Active` permits `MainWindow::saveSession()`, so prepared candidates and retired windows cannot write through hide or destruction.
Other phase transitions return `InvalidState`.

## Commands and transitions

### Startup selection

Startup loads global application session state.
When `lastLibraryPath` is non-empty and currently exists, GTK selects it, derives its default database path, and determines whether a bootstrap scan is needed.

Otherwise GTK creates and selects `<temporary-directory>/aobus-empty` with its derived database path.
The empty fallback is an application bootstrap workspace, not a persisted replacement for the user's last path until ordinary session save occurs.

### Pair construction and restoration

GTK creates a main-context executor, a per-library workspace `ConfigStore`, and `AppRuntime` with the selected root and database.
It injects the application-global playback-session store separately, registers platform audio providers, constructs `MainWindow`, and attaches runtime ownership to the window.

Window construction loads window, output, theme, layout preference, action, and controller state needed before runtime session preparation.
Preparation rebuilds library pages, restores workspace, creates a default All Tracks view when necessary, refreshes exported actions, and loads the shell layout.
It does not restore playback, start MPRIS, join the application, present the window, or request app/workspace/playback lifecycle checkpoints.

GTK first adds the prepared startup window to the application, then activates it by restoring playback intent and starting MPRIS best-effort, and finally presents it.
Playback restoration retains its existing log-and-continue failure policy.

### Open the active root

Open Library normalizes the selected directory to an absolute lexical path.
When it equals the current runtime root, GTK keeps the existing pair, optionally requests a bootstrap scan, and presents the existing window.

### Replace with another root

For a different valid directory, GTK performs:

1. Construct and prepare a candidate pair in a local owning reference while the old pair remains active.
2. Install the candidate's Open Library callback.
3. Destroy only the candidate if construction or post-construction configuration throws.
4. Call `MainWindow::retireForLibrarySwitch()` on the old pair to checkpoint, discard its restorable playback payload, and retire it.
5. Destroy only the candidate and retain the active old pair when retirement returns failure.
6. Add the candidate to the application, activate it with `StartIdle`, present it, and replace the active slot.
7. Remove and release the old pair, preserving frontend-observer-before-runtime destruction.
8. Load global application session state, replace `lastLibraryPath`, and save it best-effort.
9. Request a bootstrap scan when selected by path policy.

Replacement activation never restores the global playback payload and therefore cannot interpret old-library track or list identities.
Playback starts Idle.
MPRIS activation failures are logged and do not turn the committed replacement into a recoverable rollback.

The Open Library callback schedules this replacement through one GLib idle callback.
The dialog/portal callback therefore returns before it can trigger destruction of its owning window and coordinator.

### Retire the old pair

Retirement is idempotent after success.
On its first call it requests the ordinary window/session checkpoint, then removes the current `playback-session` group through `AppRuntime::discardRestorablePlaybackSession()`.

If discard fails, retirement returns that failure and leaves the old pair `Active`, usable, and able to save or retry.
If discard succeeds, the window enters `Retired` and all later save triggers become no-ops.

### Ordinary save and shutdown

An active window requests session save on explicit save, hide, destruction, and application release.
The coordinator captures window geometry, per-library column/presentation state, global active-library/output session values, playback session, and workspace session through their respective owners.

Application quit and handled `SIGINT` or `SIGTERM` exit through the GTK quit path.
The release helper gives the active window a save opportunity, removes it from the application, and releases it.

## Failure and cancellation

Selecting a path that is not a directory is a no-op.
Selecting the active root can still request a scan; scan failure and progress belong to the library task contract.

Playback-session discard failure aborts replacement and keeps the old pair active.
The old window presents the discard diagnostic in a parent-bound transient message and returns the same failure to the replacement callback.
Candidate preparation/configuration exceptions destroy the candidate during stack unwinding and reach the existing GLib signal exception boundary, which presents the error against the still-active old window.
Current global, workspace, layout, and several other save wrappers contain void or best-effort paths, so their failure does not currently abort replacement or shutdown.
Playback checkpoint failure is logged by the coordinator.
After retirement succeeds, playback restore and MPRIS activation retain their expected log-and-continue behavior and cannot roll the lifecycle back.
There is no rollback contract for an unexpected invariant or platform exception after retirement: the old playback payload has already been discarded, so the old pair cannot be made active again reliably.
Selected-path persistence failure is logged after commit; the new pair remains active while disk retains the previous path.

The file-dialog callback silently consumes expected cancellation or dismissal; other native chooser failures are logged, presented in a parent-bound transient message, and create no replacement.
Every native completion must enter the callback scope owned by its `ImportExportCoordinator` before it can hand a path to this lifecycle.
Coordinator teardown closes that scope before requesting native cancellation, so a late completion performs no handoff.
The replacement handoff uses one idle registration.
Shutdown first closes callback admission, whose close action cancels that pending idle registration, and only then saves and releases the active pair.

Unexpected exceptions during final release save are caught and logged so window release can continue.
Runtime-internal worker and audio quiescence belong to the [runtime execution](../../architecture/runtime-execution.md) and [playback](../../architecture/playback.md) architectures.

## Persistence and versioning

Global application session state records `lastLibraryPath` independently from the per-library workspace file.
The exact global fields belong to the [application managed-state surface](../../reference/persistence/application-config.md), and paths belong to [managed file locations](../../reference/persistence/location.md).

Workspace restore/save behavior belongs to the [workspace session specification](../workspace/session.md).
Playback restore/save/discard behavior belongs to playback.
Changing the selected root changes lifecycle association and store composition even when no serialized schema changes.

## Frontend observations

Successful same-root open presents the existing window.
Successful different-root open prepares a hidden candidate, activates it with idle playback, removes the old window, and leaves the application with only the new window.
There is no public intermediate switching snapshot or progress model.

Preparation failure is visible in a parent-bound transient message and leaves the old window/runtime and visible library in place.
Bootstrap scanning reports through the runtime library task and notification surfaces after the new pair is active.

## Implementation map

- [`app/linux-gtk/main.cpp`](../../../app/linux-gtk/main.cpp) owns startup path resolution, open callbacks, active-slot storage, application actions, signals, and shutdown release.
- [`LibraryWindowLifecycle`](../../../app/linux-gtk/app/LibraryWindowLifecycle.h) owns pair preparation, application activation, same-root reuse, and different-root replacement ordering through narrow callbacks.
- [`MainWindow`](../../../app/linux-gtk/app/MainWindow.h) and [`MainWindow.cpp`](../../../app/linux-gtk/app/MainWindow.cpp) own lifecycle phases, activation mode, retirement, hide, and destruction save triggers.
- [`MainWindowCoordinator`](../../../app/linux-gtk/app/MainWindowCoordinator.h) and [`MainWindowCoordinator.cpp`](../../../app/linux-gtk/app/MainWindowCoordinator.cpp) own prepare, optional playback restore, and checkpoint sequencing inside a pair.
- [`AppRuntime`](../../../app/include/ao/rt/AppRuntime.h) owns the interactive runtime graph and playback-session discard command.
- [`ImportExportCoordinator`](../../../app/linux-gtk/portal/ImportExportCoordinator.h) owns the platform file-dialog entry and callback handoff.
- [`ImportExportCoordinatorPolicy`](../../../app/linux-gtk/portal/ImportExportCoordinatorPolicy.h) owns default database paths and bootstrap-scan selection.

## Test map

- [`MainWindowTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowTest.cpp) proves phase transitions, candidate isolation, activation, finalization, save triggers, retirement, discard failure, and prevention of stale path writes.
- [`LibraryWindowLifecycleTest.cpp`](../../../test/unit/linux-gtk/app/LibraryWindowLifecycleTest.cpp) proves candidate failure isolation, exact replacement order, same-root reuse, persistence timing/failure, idle activation, and scan timing.
- [`MainWindowCoordinatorTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowCoordinatorTest.cpp) proves global session preservation, workspace/playback initialization, and checkpoint composition.
- [`AppConfigStoreTest.cpp`](../../../test/unit/linux-gtk/app/AppConfigStoreTest.cpp) proves global session storage behavior.
- [`ImportExportCoordinatorTest.cpp`](../../../test/unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp) protects callback-scope teardown, native cancellation, default database paths, bootstrap-scan policy, and open-callback forwarding.
- [`AppRuntimeTest.cpp`](../../../test/unit/runtime/AppRuntimeTest.cpp) protects runtime service and teardown lifetime below the GTK pair.

## Related documents

- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [Workspace session](../workspace/session.md)
- [Workspace navigation](../workspace/navigation.md)
- [Playback architecture](../../architecture/playback.md)
- [Library architecture](../../architecture/library.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Managed file locations](../../reference/persistence/location.md)
