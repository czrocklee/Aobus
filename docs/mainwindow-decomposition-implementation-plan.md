# MainWindow Decomposition Implementation Plan

Date: 2026-04-30

## Goal

Decompose `app/platform/linux/ui/MainWindow.h` and `app/platform/linux/ui/MainWindow.cpp` into smaller, coherent components without changing user-visible behavior.

This plan turns the architectural direction into an implementation checklist that can be executed in small, reviewable commits.

## Current Problem Summary

`MainWindow` currently owns too many unrelated responsibilities:

- library open/import/export orchestration
- sidebar tree construction and list CRUD
- track page graph construction
- tag editing UI and tag mutation flow
- playback setup and playback state polling
- window/session persistence
- top-level layout and status updates

The most important observation is that the real coupling center is not any single feature area. It is the combination of:

- the current library runtime state
- the current track page graph

Those two seams should be extracted first. After that, the feature-specific controllers become much easier to define cleanly.

## Design Principles

Follow these principles throughout the refactor:

1. Keep each step buildable.
2. Keep each step behavior-preserving unless a bug fix is explicitly intended.
3. Prefer moving existing code behind a better boundary before rewriting logic.
4. Avoid introducing generic framework-style abstractions.
5. Use explicit dependencies instead of letting new classes reach back into `MainWindow` internals.

## Target Architecture

The intended end state is:

- `MainWindow` becomes a thin shell for layout assembly and top-level wiring.
- `LibrarySession` owns the active library runtime state.
- `TrackPageGraph` owns track page construction and lifecycle.
- `PlaybackCoordinator` owns playback UI state and transport orchestration.
- `ImportExportCoordinator` owns file dialogs, background tasks, and import/export progress flow.
- `ListSidebarController` owns the sidebar tree, selection model, and list CRUD UI.
- `TagEditController` owns tag popovers and tag mutation operations.
- `SessionPersistence` or a small equivalent helper owns app-config serialization.

## Dependency Strategy

Extract the foundation before the feature controllers.

Recommended order:

1. `LibrarySession`
2. `TrackPageGraph`
3. `PlaybackCoordinator`
4. `ImportExportCoordinator`
5. `ListSidebarController`
6. `TagEditController`
7. `SessionPersistence`

That order matters because playback, sidebar actions, and tag editing all currently depend on the page graph and the active library state.

## Step 1: Extract `LibrarySession`

## Objective

Group the active library runtime state into one owned object so that opening or replacing a library becomes a single operation.

## State To Move

Move these members out of `MainWindow` into a dedicated struct or class:

- `_musicLibrary`
- `_rowDataProvider`
- `_allTrackIds`
- `_smartListEngine`

These are currently declared in `app/platform/linux/ui/MainWindow.h` and are created together in:

- `openMusicLibrary()`
- `importFilesFromPath()`

## Suggested Shape

```cpp
struct LibrarySession final
{
  std::unique_ptr<ao::library::MusicLibrary> musicLibrary;
  std::unique_ptr<TrackRowDataProvider> rowDataProvider;
  std::unique_ptr<ao::model::AllTrackIdsList> allTrackIds;
  std::unique_ptr<ao::model::SmartListEngine> smartListEngine;
};
```

Add a small factory or builder function:

```cpp
std::unique_ptr<LibrarySession> makeLibrarySession(std::filesystem::path const& rootPath);
```

## Implementation Checklist

- Add `LibrarySession.h` in `app/platform/linux/ui/` or another UI-adjacent location.
- Define the new state holder type.
- Add a factory function that builds a ready-to-use session from a library path.
- Replace direct `MainWindow` ownership with `std::unique_ptr<LibrarySession> _librarySession;`.
- Update all current uses of `_musicLibrary`, `_rowDataProvider`, `_allTrackIds`, and `_smartListEngine` to go through `_librarySession`.
- Convert `openMusicLibrary()` to construct a new session first and only swap state after the session is ready.
- Convert `importFilesFromPath()` to use the same session creation path.

## Files Likely Touched

- `app/platform/linux/ui/MainWindow.h`
- `app/platform/linux/ui/MainWindow.cpp`
- `app/platform/linux/ui/LibrarySession.h`
- optionally `app/platform/linux/ui/LibrarySession.cpp`

## Commit Boundary

This should be a mostly mechanical refactor. Do not change behavior, ownership rules, or UI flow in this step.

## Done Criteria

- `MainWindow` no longer directly owns the four library runtime members.
- Opening a library and creating a new imported library both use the same session construction path.
- Build passes with no behavior changes.

## Step 2: Extract `TrackPageGraph`

## Objective

Move track page creation, lookup, and lifecycle management out of `MainWindow` so that feature controllers no longer reach directly into `_trackPages` and `_stack` internals.

## State And Behavior To Move

Move the following out of `MainWindow`:

- `_trackPages`
- `clearTrackPages()`
- `rebuildListPages()`
- `buildPageForAllTracks()`
- `buildPageForStoredList()`
- `currentVisibleTrackPageContext()`
- `currentVisibleTrackPageContext() const`
- `bindTrackPagePlayback()`

Keep `Gtk::Stack _stack` in `MainWindow`, but let the new object manage the pages inside that stack.

## Key Boundary Decision

Do not move sidebar tree building into `TrackPageGraph` in this step. Keep the first extraction focused on pages, not sidebar widgets.

## Suggested Shape

```cpp
class TrackPageGraph final
{
public:
  struct Callbacks final
  {
    std::function<void(std::vector<ao::TrackId> const&)> onSelectionChanged;
    std::function<void(TrackViewPage&, double, double)> onContextMenuRequested;
    std::function<void(TrackViewPage&, std::vector<ao::TrackId> const&, double, double)> onTagEditRequested;
    std::function<void(TrackViewPage&, ao::TrackId)> onTrackActivated;
  };

  TrackPageGraph(Gtk::Stack& stack, TrackColumnLayoutModel& layoutModel, Callbacks callbacks);

  void clear();
  void rebuild(LibrarySession& session, ao::lmdb::ReadTransaction& txn);
  TrackPageContext* find(ao::ListId listId);
  TrackPageContext const* find(ao::ListId listId) const;
  TrackPageContext* currentVisible();
  TrackPageContext const* currentVisible() const;
  void show(ao::ListId listId);
};
```

## Implementation Checklist

- Add `TrackPageGraph.h` and `TrackPageGraph.cpp`.
- Move page graph creation and teardown code into the new class.
- Keep `TrackPageContext` where it is only if that keeps the change smaller. If needed later, move it into the new header in a follow-up step.
- Pass callbacks from `MainWindow` for selection changes, tag popover requests, and track activation.
- Replace `MainWindow::rebuildListPages(txn)` with `_trackPageGraph->rebuild(*_librarySession, txn)`.
- Replace direct `_trackPages` accesses in `MainWindow` with graph queries.
- Replace direct `_stack.set_visible_child(...)` calls that target list pages with `_trackPageGraph->show(listId)` where practical.

## Files Likely Touched

- `app/platform/linux/ui/MainWindow.h`
- `app/platform/linux/ui/MainWindow.cpp`
- `app/platform/linux/ui/TrackPageGraph.h`
- `app/platform/linux/ui/TrackPageGraph.cpp`

## Commit Boundary

Keep sidebar tree construction in `MainWindow` for now. The goal is to isolate the page graph without also rewriting the sidebar.

## Done Criteria

- `MainWindow` no longer directly owns `_trackPages`.
- Page creation and teardown are centralized in `TrackPageGraph`.
- Existing page-level signals still work.
- Build passes and visible behavior is unchanged.

## Step 3: Extract `PlaybackCoordinator`

## Objective

Move playback state, playback polling, and transport actions out of `MainWindow` so playback becomes a self-contained subsystem with a narrow host interface.

## State And Behavior To Move

Move the following methods:

- `setupPlayback()`
- `refreshPlaybackBar()`
- `onPlayRequested()`
- `onPauseRequested()`
- `onStopRequested()`
- `onSeekRequested()`
- `playCurrentSelection()`
- `pausePlayback()`
- `stopPlayback()`
- `seekPlayback()`
- `startPlaybackFromVisiblePage()`
- `startPlaybackSequence()`
- `playTrackAtSequenceIndex()`
- `jumpToPlayingList()`
- `clearActivePlaybackSequence()`
- `handlePlaybackFinished()`

Move the following members:

- `_playbackBar`
- `_dispatcher`
- `_playbackController`
- `_playbackTimer`
- `_activePlaybackSequence`
- `_lastPlaybackState`
- `_lastPlaybackErrorMessage`

## Boundary Warning

Do not move `onOutputChanged()` yet if it still writes directly to `_appConfig`. That persistence coupling is better handled together with the later session-persistence step.

## Suggested Host Interface

Use a small explicit interface rather than letting the coordinator hold `MainWindow&`.

```cpp
class IPlaybackHost
{
public:
  virtual ~IPlaybackHost() = default;
  virtual TrackPageContext const* currentVisibleTrackPageContext() const = 0;
  virtual TrackPageContext* findTrackPageContext(ao::ListId listId) = 0;
  virtual void showListPage(ao::ListId listId) = 0;
  virtual void updatePlaybackStatus(ao::audio::PlaybackSnapshot const& snapshot) = 0;
  virtual void showPlaybackMessage(std::string const& message,
                                   std::optional<std::chrono::seconds> timeout = std::nullopt) = 0;
};
```

`MainWindow` can implement this interface by delegating page lookups to `TrackPageGraph` and status updates to `StatusBar`.

## Implementation Checklist

- Add `PlaybackCoordinator.h` and `PlaybackCoordinator.cpp`.
- Move playback initialization into the new class constructor.
- Move the GTK polling timer ownership into the new class destructor.
- Expose a `PlaybackBar& widget()` or equivalent accessor for layout assembly.
- Route playback bar signals to coordinator methods instead of `MainWindow` methods.
- Delegate page lookup and page activation through `IPlaybackHost`.
- Keep output-device persistence in `MainWindow` until the session-persistence step.

## Files Likely Touched

- `app/platform/linux/ui/MainWindow.h`
- `app/platform/linux/ui/MainWindow.cpp`
- `app/platform/linux/ui/PlaybackCoordinator.h`
- `app/platform/linux/ui/PlaybackCoordinator.cpp`

## Commit Boundary

This step should not change playback semantics. It is a boundary extraction only.

## Done Criteria

- `MainWindow` no longer owns the playback timer and transport state.
- `MainWindow` only places the playback widget in the layout and serves as host.
- Playback UI, next-track progression, and now-playing jump still work.

## Step 4: Extract `ImportExportCoordinator`

## Objective

Move folder/file dialogs, background import/export tasks, progress dialog ownership, and completion callbacks out of `MainWindow`.

## State And Behavior To Move

Move the following methods:

- `openLibrary()`
- `importFiles()`
- `onImportFolderSelected()`
- `executeImportTask()`
- `onImportProgress()`
- `onImportFinished()`
- `scanDirectory()`
- `exportLibrary()`
- `onExportModeConfirmed()`
- `onExportFileSelected()`
- `executeExportTask()`
- `importLibrary()`

Move the following members:

- `_importWorker`
- `_importThread`
- `_importDialog`

## Key Boundary Decision

The coordinator should not directly own page rebuild logic or `MainWindow` state replacement. It should return results and notifications through callbacks.

## Suggested Callback Shape

```cpp
struct ImportExportCallbacks final
{
  std::function<void(std::unique_ptr<LibrarySession>)> onLibrarySessionCreated;
  std::function<void()> onLibraryDataMutated;
  std::function<void(double, std::string const&)> onProgressUpdated;
  std::function<void(std::string const&)> onStatusMessage;
};
```

## Implementation Checklist

- Add `ImportExportCoordinator.h` and `ImportExportCoordinator.cpp`.
- Move directory scanning into the coordinator.
- Move import progress dialog creation and lifetime into the coordinator.
- Move worker-thread ownership into the coordinator.
- Route successful library creation through `onLibrarySessionCreated`.
- Route YAML import completion through `onLibraryDataMutated`.
- Route export/import status text through a host callback.
- Add or reuse a single `MainWindow::installLibrarySession(...)` path so that open and import converge on the same state swap.

## Files Likely Touched

- `app/platform/linux/ui/MainWindow.h`
- `app/platform/linux/ui/MainWindow.cpp`
- `app/platform/linux/ui/ImportExportCoordinator.h`
- `app/platform/linux/ui/ImportExportCoordinator.cpp`

## Commit Boundary

Do not redesign import progress UI or background-thread behavior here. Keep the current user-visible flow intact.

## Done Criteria

- `MainWindow` no longer owns the import thread, import worker, or import dialog.
- Open/import/export actions are routed through the coordinator.
- Opening an existing library and importing a new one both install a `LibrarySession` through one path.

## Step 5: Extract `ListSidebarController`

## Objective

Move sidebar tree models, row binding, selection handling, context menu state, and list CRUD dialogs out of `MainWindow`.

## State And Behavior To Move

Move the following methods:

- `setupSidebarListItem()`
- `bindSidebarListItem()`
- `showListContextMenu()`
- `openNewListDialog()`
- `openNewSmartListDialog()`
- `openEditListDialog()`
- `listHasChildren()`
- `createList()`
- `selectSidebarList()`
- `updateList()`
- `onDeleteList()`
- `onEditList()`
- `buildListTree()`

Move the following members:

- `_listView`
- `_listScrolledWindow`
- `_listContextMenu`
- `_listTreeStore`
- `_treeListModel`
- `_listSelectionModel`
- `_nodesById`
- `_newListAction`
- `_deleteListAction`
- `_editListAction`

## Boundary Warning

Do not let this controller become responsible for page construction. It should own the sidebar and list actions, not the whole page graph.

## Suggested Interface

```cpp
class ListSidebarController final
{
public:
  struct Callbacks final
  {
    std::function<void(ao::ListId)> onListSelected;
    std::function<void()> onListsChanged;
    std::function<void(ao::ListId)> onListCreatedAndSelected;
  };

  Gtk::Widget& widget();
  void rebuildTree(LibrarySession& session, ao::lmdb::ReadTransaction& txn);
  void select(ao::ListId listId);
};
```

## Implementation Checklist

- Add `ListSidebarController.h` and `ListSidebarController.cpp`.
- Move all sidebar row factory and binding code into the controller.
- Move the sidebar context menu and related actions into the controller.
- Move list create/edit/delete dialog code into the controller.
- Keep tree rebuild separate from page rebuild even if both are triggered together by `MainWindow`.
- Normalize post-mutation behavior so list create/edit/delete all go through a consistent refresh path.
- Route selection changes back to `MainWindow` via `onListSelected(listId)`.

## Files Likely Touched

- `app/platform/linux/ui/MainWindow.h`
- `app/platform/linux/ui/MainWindow.cpp`
- `app/platform/linux/ui/ListSidebarController.h`
- `app/platform/linux/ui/ListSidebarController.cpp`

## Commit Boundary

This step may expose current refresh inconsistencies. Fix only the ones required to make the boundary coherent.

## Done Criteria

- `MainWindow` no longer owns the sidebar tree model stack.
- List selection changes route through the controller.
- List create/edit/delete still work and still select the correct page afterward.

## Step 6: Extract `TagEditController`

## Objective

Move tag popover creation and tag mutation application out of `MainWindow`, and replace the current implicit page-selection dependency with an explicit selection context.

## State And Behavior To Move

Move the following methods:

- `setupTrackContextMenu()`
- `showTrackContextMenu()`
- `showTagEditor()`
- `addTagToCurrentSelection()`
- `removeTagFromCurrentSelection()`
- `applyTagChangeToCurrentSelection()`

Move the following members:

- `_trackTagAddAction`
- `_trackTagRemoveAction`
- `_trackTagToggleAction`

## Key Boundary Fix

Today the flow creates tag UI with explicit selected IDs but later applies changes by re-reading `currentVisibleTrackPageContext()`. Replace that implicit dependency with an explicit selection object.

## Suggested Selection Context

```cpp
struct TrackSelectionContext final
{
  ao::ListId listId;
  std::vector<ao::TrackId> selectedIds;
  ao::model::TrackIdList* membershipList = nullptr;
};
```

## Suggested Interface

```cpp
class TagEditController final
{
public:
  void showTrackContextMenu(TrackViewPage& page,
                            TrackSelectionContext const& selection,
                            double x,
                            double y);

  void showTagEditor(TrackViewPage& page,
                     TrackSelectionContext const& selection,
                     double x,
                     double y);
};
```

## Implementation Checklist

- Add `TagEditController.h` and `TagEditController.cpp`.
- Introduce `TrackSelectionContext`.
- Update tag popover launch paths to build an explicit selection context.
- Update tag mutation logic to operate on that explicit selection instead of re-querying the current page.
- Keep status-bar messages and row-data invalidation behavior unchanged.

## Files Likely Touched

- `app/platform/linux/ui/MainWindow.h`
- `app/platform/linux/ui/MainWindow.cpp`
- `app/platform/linux/ui/TagEditController.h`
- `app/platform/linux/ui/TagEditController.cpp`

## Commit Boundary

Do not redesign tag UI behavior here. Only improve the ownership and data flow boundary.

## Done Criteria

- `MainWindow` no longer owns tag-edit actions.
- Tag changes apply to the explicit selected IDs from the initiating page.
- Existing status updates and row invalidation behavior remain correct.

## Step 7: Extract `SessionPersistence`

## Objective

Move app-config load/save and window-state persistence out of `MainWindow` once the bigger structural seams are already in place.

## State And Behavior To Move

Move the following:

- `_appConfig`
- `saveSession()`
- `loadSession()`

## Implementation Checklist

- Add `SessionPersistence.h` and optionally `SessionPersistence.cpp`.
- Move `AppConfig` load/save behavior into a small helper class or focused free functions.
- Keep the persisted state format unchanged.
- Decide whether playback output selection persistence should move here together with `onOutputChanged()`.
- Make `MainWindow` ask the helper to restore window geometry, last library path, and track view layout.

## Files Likely Touched

- `app/platform/linux/ui/MainWindow.h`
- `app/platform/linux/ui/MainWindow.cpp`
- `app/platform/linux/ui/SessionPersistence.h`
- optionally `app/platform/linux/ui/SessionPersistence.cpp`

## Commit Boundary

Keep this small. It is cleanup after the larger architectural seams are already in place.

## Done Criteria

- `MainWindow` no longer directly owns `AppConfig` persistence logic.
- Session restore and save behavior remain unchanged.

## Cross-Step Validation Checklist

After each step:

- Build the project.
- Open an existing library.
- Import a new library from a directory.
- Switch between sidebar lists.
- Verify the visible page changes correctly.
- Verify cover art updates when selection changes.
- Start playback from the selected row.
- Pause, seek, stop, and resume playback.
- Use the status bar now-playing jump.
- Edit tags from a track selection.
- Create, edit, and delete a list.
- Quit and relaunch to verify session persistence still works.

## Suggested Commit Sequence

1. `Extract LibrarySession from MainWindow state`
2. `Extract TrackPageGraph page lifecycle from MainWindow`
3. `Extract PlaybackCoordinator from MainWindow`
4. `Extract ImportExportCoordinator threading and dialogs`
5. `Extract ListSidebarController sidebar tree and list actions`
6. `Extract TagEditController and explicit track selection context`
7. `Extract SessionPersistence from MainWindow`

## End-State Responsibility Split

At the end of this plan, `MainWindow` should mainly be responsible for:

- top-level widget layout
- constructing owned subcomponents
- wiring component callbacks together
- installing a new `LibrarySession`
- window-level status and cover-art presentation

It should no longer directly own the implementation details of playback, import/export threading, sidebar models, or track page graph construction.
