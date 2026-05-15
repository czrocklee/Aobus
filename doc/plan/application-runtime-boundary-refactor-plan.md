# Application Runtime Boundary Refactor Plan

## Purpose

This plan turns the application/runtime boundary design into concrete, staged
work. It focuses on small, verifiable changes that improve naming, ownership,
and class responsibility without interrupting ongoing feature development.

Design contract: `doc/design/application-runtime-boundaries.md`.

## Success Criteria

- Main runtime/container names match their roles.
- GTK window orchestration no longer owns platform bootstrap or persistence
  details that can be isolated.
- Workspace navigation state is separated from session persistence.
- Layout component dependencies are passed through an explicit context with a
  stable role name.
- CLI/GTK shared behavior has a path toward frontend-neutral command/query
  services.
- Each phase can be reviewed and tested independently.

## Phase 0: Baseline and Guardrails

### Work

- Confirm the current build is green before mechanical refactors.
- Record any existing failing tests or known warnings before starting.
- Avoid mixed behavior changes during pure rename phases.

### Verification

```bash
./build.sh debug
```

If the full build is too slow during iteration, use a targeted CMake build and
run `ao_test` before merging.

## Phase 1: Mechanical Role Renames

### Intent

Make class names match their existing roles before deeper extraction.

### Changes

1. Rename runtime container:
   - `app/runtime/AppSession.h` -> `app/runtime/AppRuntime.h`
   - `app/runtime/AppSession.cpp` -> `app/runtime/AppRuntime.cpp`
   - `rt::AppSession` -> `rt::AppRuntime`
   - `rt::AppSessionDependencies` -> `rt::AppRuntimeDependencies`

2. Rename GTK main coordinator:
   - `app/linux-gtk/app/WindowController.h` ->
     `app/linux-gtk/app/MainWindowCoordinator.h`
   - `app/linux-gtk/app/WindowController.cpp` ->
     `app/linux-gtk/app/MainWindowCoordinator.cpp`
   - `gtk::WindowController` -> `gtk::MainWindowCoordinator`

3. Rename layout context:
   - `gtk::layout::LayoutDependencies` -> `gtk::layout::LayoutContext`
   - `LayoutDependencies.h` -> `LayoutContext.h`
   - Update includes and field names only where needed.

4. Rename track page host:
   - `TrackPageManager` -> `TrackPageHost`
   - `TrackPageManager.h/.cpp` -> `TrackPageHost.h/.cpp`

5. Update CMake source lists and all includes.

### Constraints

- Do not change behavior in this phase.
- Keep compatibility aliases only if the rename creates an unreasonably large
  migration. Otherwise update all references directly.

### Verification

```bash
./build.sh debug
```

## Phase 2: Extract GTK Platform Bootstrap

### Intent

Move Linux audio provider registration out of the main window coordinator.

### Current Issue

The current window controller registers PipeWire and ALSA providers during
session initialization. Audio backend discovery is platform bootstrap, not
window orchestration.

### Changes

1. Add a small GTK platform bootstrap helper, for example:

   ```text
   app/linux-gtk/platform/AudioBackendBootstrap.h
   app/linux-gtk/platform/AudioBackendBootstrap.cpp
   ```

2. Provide one function:

   ```cpp
   namespace ao::gtk
   {
     void registerPlatformAudioBackends(rt::AppRuntime& runtime);
   }
   ```

3. Move `PIPEWIRE_FOUND` and `ALSA_FOUND` provider registration into that
   helper.

4. Call the helper from GTK startup or from the main window coordinator startup
   path. Prefer GTK application bootstrap if the runtime is available there;
   otherwise use the coordinator as a temporary caller while keeping the logic
   out of the coordinator class.

### Tests

- Existing playback service tests should continue to pass.
- If the helper can be compiled under both provider-enabled and provider-disabled
  builds, add no runtime test unless the build matrix already supports provider
  toggles.

### Verification

```bash
./build.sh debug
```

## Phase 3: Extract Window and Track View Persistence

### Intent

Remove persistence details from the main window coordinator.

### Changes

1. Add a small helper in the GTK app layer:

   ```text
   app/linux-gtk/app/WindowStatePersistence.h
   app/linux-gtk/app/WindowStatePersistence.cpp
   ```

2. Move these responsibilities into it:

   - load/save `WindowState`
   - load/save `TrackViewState`
   - conversion between `TrackViewState` and `TrackColumnLayout`

3. Keep the coordinator responsible only for deciding when to load/save.

4. Preserve existing config keys:

   - `window`
   - `track_view`

### Suggested API

```cpp
namespace ao::gtk
{
  class WindowStatePersistence final
  {
  public:
    explicit WindowStatePersistence(rt::ConfigStore& configStore);

    void loadWindow(Gtk::Window& window) const;
    void saveWindow(Gtk::Window const& window) const;

    void loadTrackView(TrackColumnLayoutModel& model) const;
    void saveTrackView(TrackColumnLayout const& layout) const;
  };
}
```

The exact API may differ, but it should keep config serialization outside the
main window coordinator.

### Tests

- Unit-test conversion between `TrackViewState` and `TrackColumnLayout` if it is
  extracted into testable free functions.
- Keep GTK widget load/save covered by existing integration/manual checks unless
  the test suite already supports GTK widget construction.

### Verification

```bash
./build.sh debug
```

## Phase 4: Separate Workspace State from Session Persistence

### Intent

Make workspace navigation a runtime model and move config-backed persistence out
of `WorkspaceService`.

### Changes

1. Introduce a persistence collaborator, for example:

   ```text
   app/runtime/SessionPersistenceService.h
   app/runtime/SessionPersistenceService.cpp
   ```

2. Move `restoreSession()` and `saveSession()` implementation details from
   `WorkspaceService` into the new service.

3. Keep `WorkspaceService` responsible for:

   - focused view state
   - opening and closing views
   - navigation targets
   - exposing `LayoutState`

4. Let the persistence service depend on the services it needs:

   - `WorkspaceService`
   - `ViewService`
   - `PlaybackService`
   - `ConfigStore`

5. Update the runtime container to construct the persistence service after its
   dependencies.

6. Decide whether compatibility methods remain on `WorkspaceService` during a
   transition:

   - Short transition: `WorkspaceService::restoreSession()` delegates to the
     persistence service through the runtime container caller.
   - Final state: callers use `runtime.sessionPersistence().restore()`.

### Tests

- Add focused tests for persistence snapshot restore/save if existing runtime
  tests already construct services.
- Cover at least one restored open-view/focused-view scenario and one default
  empty-session scenario.

### Verification

```bash
./build.sh debug
```

## Phase 5: Replace Layout Service Bag Wiring

### Intent

Make GTK layout service wiring explicit and reduce field-by-field mutation from
`MainWindow`.

### Changes

1. Introduce a grouped services object in the GTK app layer:

   ```cpp
   namespace ao::gtk
   {
     struct GtkUiServices final
     {
       TrackPageHost* trackPageHost = nullptr;
       TrackColumnLayoutModel* columnLayoutModel = nullptr;
       TrackRowCache* trackRowCache = nullptr;
       ListSidebarController* listSidebarController = nullptr;
       PlaybackSequenceController* playbackSequenceController = nullptr;
       CoverArtCache* coverArtCache = nullptr;
       TagEditController* tagEditController = nullptr;
       ImportExportCoordinator* importExportCoordinator = nullptr;
     };
   }
   ```

2. Let `MainWindowCoordinator` produce `GtkUiServices` after initialization.

3. Add a `ShellLayoutController::setServices(GtkUiServices const&)` or
   `LayoutContext::bind(GtkUiServices const&)` method.

4. Replace manual field assignments in `MainWindow::initializeSession()` with a
   single bind call.

5. Keep `ShellUiContext::menuModel` separate if it is created by
   `MenuController`, or include it in a broader shell services struct.

### Tests

- Existing GTK smoke/build coverage is likely sufficient unless layout component
  construction tests already exist.

### Verification

```bash
./build.sh debug
```

## Phase 6: Introduce Frontend-Neutral CLI/GTK Application Services

### Intent

Create a path for CLI and GTK to share use cases without forcing CLI into the
GUI workspace runtime.

### First Candidate Services

Start with the smallest duplicate-prone area. Recommended order:

1. `TrackCommandService`
   - update metadata
   - edit tags
   - remove or import track-related operations if already shared

2. `ListCommandService`
   - create/update/delete lists
   - smart-list draft validation

3. `LibraryQueryService`
   - common listing and lookup operations needed by CLI and GTK adapters

### Placement

Prefer a new frontend-neutral directory if the split is clear:

```text
app/application
```

If that is too disruptive initially, add the services under `app/runtime` with a
clear comment that they are frontend-neutral and do not depend on workspace/view
types.

### Migration Rule

- New CLI behavior should use application services.
- Existing CLI direct `MusicLibrary` usage can migrate command by command.
- GTK controllers should use these services when performing the same operations
  as CLI commands.

### Tests

- Add unit tests for each service using a temporary library root.
- Prefer tests at the service API boundary rather than duplicating CLI parser
  tests.

### Verification

```bash
./build.sh debug
```

## Phase 7: Optional Directory Split

### Intent

Only move directories after ownership is already clear from class names and
dependencies.

### Candidate Target Layout

```text
app/application
  frontend-neutral command/query services

app/runtime
  runtime container, subscriptions, notifications, common state primitives

app/runtime/workspace
  workspace, view, projection, track sources

app/linux-gtk
  GTK frontend and platform adaptation
```

### Constraints

- Do not combine directory moves with behavior changes.
- Update CMake and include paths in the same commit as the move.
- Keep public headers under `include/ao` untouched unless a type is intentionally
  promoted to public API.

### Verification

```bash
./build.sh debug
```

## Suggested Commit Order

1. Add design and plan documents.
2. Rename `AppSession` to `AppRuntime`.
3. Rename `LayoutDependencies` to `LayoutContext`.
4. Rename `TrackPageManager` to `TrackPageHost`.
5. Rename `WindowController` to `MainWindowCoordinator`.
6. Extract GTK audio backend bootstrap.
7. Extract window/track-view persistence.
8. Extract workspace/session persistence.
9. Add `GtkUiServices` layout binding.
10. Add first frontend-neutral application service and migrate one CLI command.

Each commit should build independently.

## Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Mechanical renames obscure real behavior changes. | Keep rename commits behavior-free and review with build-only verification. |
| CLI is coupled to GUI workspace concepts. | Extract frontend-neutral services instead of making CLI depend on the full runtime container. |
| Layout context becomes a global service locator. | Bind grouped UI services explicitly and keep layout component needs narrow. |
| Persistence moves accidentally change config format. | Preserve existing config keys and add conversion tests where possible. |
| Platform provider registration becomes hard to find. | Place it under a clearly named GTK platform/bootstrap file and call it from startup. |

## Definition of Done

The refactor is complete when:

- The transitional names listed in the design document have been replaced.
- Main window coordination, platform bootstrap, and persistence are separate
  concepts in code.
- Workspace state can be understood without reading config serialization code.
- Layout services are bound as an explicit grouped context.
- At least one CLI command uses a frontend-neutral application service that is
  also suitable for GTK use.
- `./build.sh debug` passes.
