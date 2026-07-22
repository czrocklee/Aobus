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
Its implementation is `app/linux-gtk/main.cpp`, `app/linux-gtk/app/MainWindow.cpp`, and `MainWindowCoordinator.cpp`.

GTK may select platform paths, construct stores and executors, register audio providers, own windows, and coordinate lifecycle commands.
It cannot redefine runtime workspace, playback, library, or persistence payload semantics.

## Terminology

- **Active library** is the music root bound to the current main-window `AppRuntime`.
- **Window/runtime pair** is one `MainWindow` and the `AppRuntime` whose lifetime is attached to it.
- **Prepare for switch** is the old pair's checkpoint, playback-session discard, and stale-write guard transition.
- **Bootstrap scan** is the optional scan requested after opening a root whose database policy requires it.
- **Empty fallback root** is the temporary `aobus-empty` directory used when no saved existing root can be opened at startup.

## Invariants

- GTK has at most one active main-window/runtime pair in ordinary application composition.
- One pair remains bound to one music root and database path for its lifetime.
- Selecting a different root replaces the complete pair; it never retargets the existing runtime or storage graph.
- Application-global configuration and shell-layout stores survive replacement; per-library runtime, database, workspace store, sources, views, and playback state do not.
- The old window is prepared before its runtime is destroyed.
- A prepared old window cannot later overwrite the new global `lastLibraryPath` during hide or destruction.
- The globally stored restorable playback session is discarded before a different library becomes active, preventing old library identities from being restored against the new root.
- Frontend observers and GTK objects are released before the window-owned runtime.
- Opening a selected root whose normalized requested path equals the current runtime root reuses the pair rather than creating a duplicate runtime.
- A native Open Library completion cannot request replacement after its owning coordinator has been destroyed.

## State model

The GTK lifecycle retains:

- an optional active `MainWindow` reference;
- application-global `AppConfigStore`, shell-layout store, and component-state store shared across pair replacement;
- global application session state containing the last selected path and output identities;
- one window-local `librarySwitchPrepared` guard;
- one runtime-local music root, database, workspace store, and playback-session owner.

`librarySwitchPrepared` begins false.
It becomes true only after save and successful playback-session discard, and makes subsequent `MainWindow::saveSession()` calls no-ops for that old pair.

## Commands and transitions

### Startup selection

Startup loads global application session state.
When `lastLibraryPath` is non-empty and currently exists, GTK selects it, derives its default database path, and determines whether a bootstrap scan is needed.

Otherwise GTK creates and selects `<temporary-directory>/aobus-empty` with its derived database path.
The empty fallback is an application bootstrap workspace, not a persisted replacement for the user's last path until ordinary session save occurs.

### Pair construction and restoration

GTK creates a main-context executor, a per-library workspace `ConfigStore`, and `AppRuntime` with the selected root and database.
It injects the application-global playback-session store separately, registers platform audio providers, constructs `MainWindow`, and attaches runtime ownership to the window.

Window construction loads window, output, theme, layout preference, action, and controller state needed before runtime session initialization.
Initialization then rebuilds library pages, restores workspace, creates a default All Tracks view when necessary, restores playback intent, refreshes exported actions, and loads the shell layout.

### Open the active root

Open Library normalizes the selected directory to an absolute lexical path.
When it equals the current runtime root, GTK keeps the existing pair, optionally requests a bootstrap scan, and presents the existing window.

### Replace with another root

For a different valid directory, GTK performs:

1. `MainWindow::prepareForLibrarySwitch()` on the old pair.
2. Abort without removing the pair when preparation returns failure.
3. Remove the old window from the application and release the last main-window reference.
4. Load global application session state, replace `lastLibraryPath`, and save it.
5. Construct and initialize a new pair for the selected root.
6. Install its Open Library callback.
7. Request a bootstrap scan when selected by path policy.

The Open Library callback schedules this replacement through one GLib idle callback.
The dialog/portal callback therefore returns before it can trigger destruction of its owning window and coordinator.

### Prepare the old pair

Preparation is idempotent after success.
On its first call it requests the ordinary window/session checkpoint, then removes the current `playback-session` group through `AppRuntime::discardRestorablePlaybackSession()`.

If discard fails, preparation returns that failure and leaves the stale-write guard false so the old pair remains active and can be retried.
If discard succeeds, preparation sets the guard true.

### Ordinary save and shutdown

An unprepared window requests session save on explicit save, hide, destruction, and application release.
The coordinator captures window geometry, per-library column/presentation state, global active-library/output session values, playback session, and workspace session through their respective owners.

Application quit and handled `SIGINT` or `SIGTERM` exit through the GTK quit path.
The release helper gives the active window a save opportunity, removes it from the application, and releases it.

## Failure and cancellation

Selecting a path that is not a directory is a no-op.
Selecting the active root can still request a scan; scan failure and progress belong to the library task contract.

Playback-session discard failure aborts replacement and keeps the old pair active.
The old window presents the discard diagnostic in a parent-bound transient message and returns the same failure to the replacement callback.
Current global, workspace, layout, and several other save wrappers contain void or best-effort paths, so their failure does not currently abort replacement or shutdown.
Playback checkpoint failure is logged by the coordinator.
[RFC 0019](../../rfc/0019-safe-active-library-replacement.md) proposes preparing the replacement pair before the old pair's final checkpoint and release, then saving the selected path only after activation.

The file-dialog callback silently consumes expected cancellation or dismissal; other native chooser failures are logged, presented in a parent-bound transient message, and create no replacement.
Every native completion must enter the callback scope owned by its `ImportExportCoordinator` before it can hand a path to this lifecycle.
Coordinator teardown closes that scope before requesting native cancellation, so a late completion performs no handoff.
The idle replacement callback has no explicit cancellation token and relies on GTK application/window lifetime.

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
Successful different-root open removes the old window and presents a newly initialized window.
There is no public intermediate switching snapshot or progress model.

Preparation failure is visible in a parent-bound transient message and leaves the old window/runtime and visible library in place.
Bootstrap scanning reports through the runtime library task and notification surfaces after the new pair is active.

## Implementation map

- [`app/linux-gtk/main.cpp`](../../../app/linux-gtk/main.cpp) owns startup path resolution, pair construction, open callbacks, active-pair replacement, application actions, signals, and release.
- [`MainWindow`](../../../app/linux-gtk/app/MainWindow.h) and [`MainWindow.cpp`](../../../app/linux-gtk/app/MainWindow.cpp) own switch preparation, the stale-write guard, hide, and destruction save triggers.
- [`MainWindowCoordinator`](../../../app/linux-gtk/app/MainWindowCoordinator.h) and [`MainWindowCoordinator.cpp`](../../../app/linux-gtk/app/MainWindowCoordinator.cpp) own current restore and checkpoint sequencing inside a pair.
- [`AppRuntime`](../../../app/include/ao/rt/AppRuntime.h) owns the interactive runtime graph and playback-session discard command.
- [`ImportExportCoordinator`](../../../app/linux-gtk/portal/ImportExportCoordinator.h) owns the platform file-dialog entry and callback handoff.
- [`ImportExportCoordinatorPolicy`](../../../app/linux-gtk/portal/ImportExportCoordinatorPolicy.h) owns default database paths and bootstrap-scan selection.

## Test map

- [`MainWindowTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowTest.cpp) proves hide and explicit save triggers, successful switch preparation, playback-session discard, and prevention of stale path writes.
- [`MainWindowCoordinatorTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowCoordinatorTest.cpp) proves global session preservation, workspace/playback initialization, and checkpoint composition.
- [`AppConfigStoreTest.cpp`](../../../test/unit/linux-gtk/app/AppConfigStoreTest.cpp) proves global session storage behavior.
- [`ImportExportCoordinatorTest.cpp`](../../../test/unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp) protects callback-scope teardown, native cancellation, default database paths, bootstrap-scan policy, and open-callback forwarding.
- [`AppRuntimeTest.cpp`](../../../test/unit/runtime/AppRuntimeTest.cpp) protects runtime service and teardown lifetime below the GTK pair.

The internal `main.cpp` replacement sequence does not currently have a focused end-to-end test; the window preparation and component policies above protect its principal state boundaries.
[RFC 0019](../../rfc/0019-safe-active-library-replacement.md) proposes private prepare/activate operations with focused transition tests.

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
- [RFC 0019: safe active-library replacement](../../rfc/0019-safe-active-library-replacement.md)
