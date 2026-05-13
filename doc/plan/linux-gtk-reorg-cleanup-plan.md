# Linux GTK Reorganization and Cleanup Plan

## Purpose

This document defines a safe, staged plan for reorganizing `app/linux-gtk` after the free-layout migration. The current `MainWindow` already delegates most visible UI construction to layout components, but the old `ui/` directory still contains legacy classes, stale includes, and responsibilities that should move into feature-oriented folders.

The goals are:

1. Remove classes that are no longer used after componentization.
2. Shrink `MainWindow` into a top-level window/composition shell.
3. Reorganize GTK code by feature domain instead of a single `ui/` bucket.
4. Preserve current behavior during each phase.
5. Make every phase independently buildable and verifiable.

## Current Verified State

The current default layout is component-driven. `createDefaultLayout()` builds the window from layout nodes such as:

- `app.menuBar`
- `playback.outputButton`
- `playback.playPauseButton`
- `playback.stopButton`
- `playback.seekSlider`
- `playback.timeLabel`
- `playback.volumeControl`
- `library.listTree`
- `inspector.coverArt`
- `app.workspaceWithInspector`
- `status.defaultBar`

The corresponding component factories are registered in `layout/components/SemanticComponents.cpp` and `layout/components/PlaybackComponents.cpp`.

`MainWindow` is still the owner/composition root for many services and controllers:

- `TrackRowDataProvider`
- `CoverArtCache`
- `StatusBar`
- `TagEditController`
- `ListSidebarController`
- `TrackPageGraph`
- `ImportExportCoordinator`
- `PlaybackController`
- `TrackColumnLayoutModel`
- `layout::ComponentRegistry`
- `layout::ComponentContext`
- `layout::LayoutHost`

The component tree accesses those dependencies through `layout::ComponentContext`.

## Dependency Double-Check Summary

The following summary was produced by checking symbol references in `app/linux-gtk`, `app/runtime`, `include`, `doc/design`, and `doc/plan`.

### Strong Deletion Candidates

These classes appear to be legacy or dead after layout componentization.

| Class / files | Current references | Assessment | Cleanup rule |
| --- | --- | --- | --- |
| `PlaybackBar` (`ui/PlaybackBar.*`) | Own files, stale `MainWindow` include/forward declaration, docs. No runtime construction. | Replaced by `playback.*` layout components. | Delete after removing stale `MainWindow` references and confirming no configurable layout still expects a monolithic playback bar component. |
| `OutputMenuModel` (`ui/OutputMenuModel.*`) | Own files, CMake, docs. No runtime users. | Replaced by inline output list model in `PlaybackComponents.cpp`. | Delete independently. |
| `TagPromptDialog` (`ui/TagPromptDialog.*`) | Own files, CMake, audit docs. No runtime users. | Replaced by `TagPopover` + `TagEditor` flow. | Delete independently. |
| `GtkMainThreadDispatcher` (`ui/GtkMainThreadDispatcher.*`) | Created only by `MainWindow`; not passed to runtime. Docs mention it. | Likely obsolete after `GtkControlExecutor` became the runtime executor. | Remove `MainWindow::_dispatcher` first; delete class after verifying no non-GTK app path still needs `ao::IMainThreadDispatcher`. |
| `MainWindow::_inspectorRevealer`, `MainWindow::_inspectorHandle`, `MainWindow::kCoverArtSize` | Only initialized or declared in `MainWindow`. | Replaced by `WorkspaceWithInspectorComponent`. | Remove from `MainWindow`. |

### Must Keep For Now

These classes are still used by active components or runtime wiring and must not be deleted in cleanup-only phases.

| Class / files | Why it is still needed |
| --- | --- |
| `GtkControlExecutor` | Created in `main.cpp` and passed into `AppSession`; runtime services depend on `IControlExecutor`. |
| `AobusSoul` | Used by `playback.outputButton` and `playback.qualityIndicator` components. |
| `VolumeBar` | Used by `playback.volumeControl`. |
| `OutputListItems` | Used by `playback.outputButton`; also included by `StatusBar`. |
| `StatusBar` | Wrapped by `status.defaultBar`. |
| `ListSidebarController` | Wrapped by `library.listTree`; also used by `TrackPageGraph` for smart-list creation from filters. |
| `ListRow`, `ListTreeNode` | Internal model objects for `ListSidebarController`. |
| `SmartListDialog`, `QueryExpressionBox` | Used by list creation/edit flow and smart-list preview. |
| `TrackPageGraph` | Owns page lifecycle and the stack used by `tracks.table` and `app.workspaceWithInspector`. |
| `TrackViewPage` | Created by `TrackPageGraph` for actual track pages. |
| `TrackListAdapter`, `TrackListModel`, `TrackRow`, `TrackRowDataProvider`, `TrackPresentation` | Required by track pages, projections, rows, and column state. |
| `InspectorSidebar` | Created by `inspector.sidebar` and `app.workspaceWithInspector`. |
| `CoverArtWidget`, `CoverArtCache` | Used by inspector components. |
| `TagEditController`, `TagPopover`, `TagEditor` | Used by track-page context menus and inspector tag editing. |
| `PlaybackController` | Used by `TrackPageGraph` to start and advance playback from a track page. |
| `ImportExportCoordinator`, `ImportProgressDialog` | Used by menu actions and import/export flows. |
| `AobusSoulWindow` | Full-screen easter egg window. Currently only wired in the old `PlaybackBar` (right-click-hold-1s on output button). Must be preserved and re-wired into `playback.outputButton` component. |

## Target Folder Structure

The end state should be feature-oriented. This plan keeps namespace changes separate from mechanical file moves to reduce risk.

Aobus uses singular directory/domain names in the rest of the repository (`library`, `model`, `query`, `tag`, `utility`, `runtime`, `service`). New GTK feature directories and namespaces should follow the same convention. Use `track/`, `list/`, and `tag/`, not `tracks/`, `lists/`, or `tags/`.

```text
app/linux-gtk/
  main.cpp
  CMakeLists.txt

  app/
    MainWindow.h/.cpp
    WindowController.h/.cpp       # Phase 5: split from MainWindow
    WindowContext.h                # Phase 5: split from MainWindow
    MenuController.h/.cpp          # Phase 5: split from MainWindow

  shell/
    ShellLayoutController.h/.cpp   # Phase 5: split from MainWindow
    StatusBar.h/.cpp
    UIState.h
    ThemeBus.h/.cpp

  common/
    GtkControlExecutor.h/.cpp
    SvgTemplate.h

  layout/
    LayoutConstants.h              # stays in layout/; used by layout components
    document/
      LayoutDocument.h/.cpp
      LayoutNode.h
      LayoutYaml.h
    runtime/
      ILayoutComponent.h           # current name preserved
      ComponentContext.h           # Phase 7: rename to LayoutDependencies.h
      ComponentRegistry.h/.cpp
      LayoutHost.h/.cpp
      LayoutRuntime.h/.cpp
    components/
      ContainerComponents.h/.cpp
      PlaybackComponents.h/.cpp
      SemanticComponents.h/.cpp
    editor/
      LayoutEditorDialog.h/.cpp

  track/
    TrackPageManager.h/.cpp        # Phase 4: rename of TrackPageGraph
    TrackViewPage.h/.cpp
    TrackListAdapter.h/.cpp
    TrackListModel.h/.cpp
    TrackPresentation.h/.cpp       # also contains TrackColumnLayoutModel
    TrackRow.h/.cpp
    TrackRowDataProvider.h/.cpp

  list/
    ListSidebarController.h/.cpp
    ListRow.h/.cpp
    ListTreeNode.h/.cpp
    SmartListDialog.h/.cpp
    QueryExpressionBox.h/.cpp

  playback/
    PlaybackController.h/.cpp      # Phase 4: rename to PlaybackSequenceController
    AobusSoul.h/.cpp
    AobusSoulWindow.h/.cpp         # Full-screen easter egg, triggered by output button long-press
    VolumeBar.h/.cpp
    OutputListItems.h

  inspector/
    InspectorSidebar.h/.cpp
    CoverArtCache.h/.cpp
    CoverArtWidget.h/.cpp

  tag/
    TagEditController.h/.cpp
    TagPopover.h/.cpp
    TagEditor.h/.cpp

  library_io/
    ImportExportCoordinator.h/.cpp
    ImportProgressDialog.h/.cpp
    PlaylistExporter.h/.cpp        # moved from service/
```

Later namespace cleanup can move classes from `ao::gtk` into feature namespaces such as `ao::gtk::track`, `ao::gtk::list`, `ao::gtk::playback`, `ao::gtk::inspector`, `ao::gtk::tag`, and `ao::gtk::library_io`.

## Phase 0: Baseline Verification

Before changing code, capture the current baseline.

### Checks

Run from the project root:

```bash
./build.sh debug
```

If full debug build is too expensive during iteration, at minimum run:

```bash
nix-shell --run "cmake --build /tmp/build/debug --target aobus-gtk-lib --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test"
```

### Acceptance Criteria

- Existing baseline is known.
- Any pre-existing failures are recorded and not attributed to the reorg.

## Phase 1: Remove Stale `MainWindow` Dependencies

This phase is low risk and should not change user-visible behavior.

### Changes

1. Remove unused `MainWindow` includes:
   - `GtkMainThreadDispatcher.h`, if `_dispatcher` is removed.
   - `InspectorSidebar.h`
   - `TrackViewPage.h`
   - `CoverArtWidget.h`
   - `ImportProgressDialog.h`
   - `PlaybackBar.h`
   - `SmartListDialog.h`
   - `TagPopover.h`
   - `TrackListAdapter.h`
   - `ListRow.h`
   - `ListTreeNode.h`
   - any other include that is not needed by declarations or definitions.

2. Remove unused `MainWindow` forward declarations:
   - `TrackListAdapter`
   - `TrackViewPage`
   - `ListRow`
   - `ListTreeNode`
   - `CoverArtWidget`
   - `ImportProgressDialog`
   - `PlaybackBar`
   - duplicate `TrackPageGraph` declaration if direct include remains necessary.

3. Remove unused `MainWindow` members:
   - `_dispatcher`
   - `_inspectorRevealer`
   - `_inspectorHandle`
   - `kCoverArtSize`

4. Remove corresponding initialization from `MainWindow.cpp`:
   - `std::make_shared<GtkMainThreadDispatcher>()`
   - `_inspectorHandle.set_active(false)`
   - `_inspectorRevealer.set_reveal_child(false)`

### Required Double Checks

```bash
rg -n "GtkMainThreadDispatcher|_dispatcher|_inspectorRevealer|_inspectorHandle|kCoverArtSize|PlaybackBar" app/linux-gtk/ui/MainWindow.* app/linux-gtk/layout app/linux-gtk/main.cpp
```

Expected:

- No `MainWindow` references to removed members.
- `AobusSoulWindow` and `AobusSoul` are NOT removed — both remain in use.
- No accidental deletion of `GtkControlExecutor` usage in `main.cpp`.

### Verification

```bash
nix-shell --run "cmake --build /tmp/build/debug --target aobus-gtk-lib --parallel"
```

## Phase 2: Delete Confirmed Dead Classes

Delete only the classes that have no active runtime users.

### 2.1 Delete `OutputMenuModel`

Files:

- `app/linux-gtk/ui/OutputMenuModel.h`
- `app/linux-gtk/ui/OutputMenuModel.cpp`

Also remove `ui/OutputMenuModel.cpp` from `app/linux-gtk/CMakeLists.txt`.

Double check:

```bash
rg -n "\bOutputMenuModel\b" app/linux-gtk app/runtime include
```

Expected: no results.

### 2.2 Delete `TagPromptDialog`

Files:

- `app/linux-gtk/ui/TagPromptDialog.h`
- `app/linux-gtk/ui/TagPromptDialog.cpp`

Also remove `ui/TagPromptDialog.cpp` from `app/linux-gtk/CMakeLists.txt`.

Double check:

```bash
rg -n "\bTagPromptDialog\b" app/linux-gtk app/runtime include
```

Expected: no results.

### 2.3 Delete `PlaybackBar`

Files:

- `app/linux-gtk/ui/PlaybackBar.h`
- `app/linux-gtk/ui/PlaybackBar.cpp`

Also remove `ui/PlaybackBar.cpp` from `app/linux-gtk/CMakeLists.txt`.

Important: do **not** remove these files/classes:

- `AobusSoul.h/.cpp`
- `VolumeBar.h/.cpp`
- `OutputListItems.h`
- `AobusSoulWindow.h/.cpp`

`AobusSoulWindow` is the full-screen easter egg (right-click-hold-1s → full-screen "AO"). It must be preserved and re-wired into the `playback.outputButton` layout component (see Phase 6 Playback domain).

Double check:

```bash
rg -n "\bPlaybackBar\b" app/linux-gtk app/runtime include
rg -n "\bAobusSoul\b|\bVolumeBar\b|\bOutputListItems\b|\bAobusSoulWindow\b" app/linux-gtk/layout app/linux-gtk/ui app/linux-gtk/CMakeLists.txt
```

Expected:

- No `PlaybackBar` code references.
- `AobusSoul.cpp`, `VolumeBar.cpp`, and `AobusSoulWindow.cpp` remain in CMake.
- `AobusSoul`, `VolumeBar`, `OutputListItems`, and `AobusSoulWindow` remain referenced.

### 2.4 Delete `GtkMainThreadDispatcher`

Only do this after Phase 1 removes `MainWindow::_dispatcher`.

Files:

- `app/linux-gtk/ui/GtkMainThreadDispatcher.h`
- `app/linux-gtk/ui/GtkMainThreadDispatcher.cpp`

Also remove `ui/GtkMainThreadDispatcher.cpp` from `app/linux-gtk/CMakeLists.txt`.

Do **not** delete `GtkControlExecutor`.

Double check:

```bash
rg -n "\bGtkMainThreadDispatcher\b|\bIMainThreadDispatcher\b" app/linux-gtk app/runtime include
rg -n "\bGtkControlExecutor\b|\bIControlExecutor\b" app/linux-gtk app/runtime include
```

Expected:

- No app code references `GtkMainThreadDispatcher`.
- `GtkControlExecutor` remains used by `main.cpp` and runtime services.
- Do **not** remove `include/ao/utility/IMainThreadDispatcher.h` as part of GTK cleanup. It is still used by `test/unit/audio/TestUtility.h` (`MockDispatcher`). Decide in a separate core cleanup phase whether to consolidate it with `IControlExecutor`.

### Verification

After each deletion subsection:

```bash
nix-shell --run "cmake --build /tmp/build/debug --target aobus-gtk-lib --parallel"
```

After all Phase 2 deletions:

```bash
./build.sh debug
```

## Phase 3: Mechanical Folder Reorganization Without Namespace Changes

This phase moves files into feature folders while keeping class names and namespaces unchanged. It should be almost purely mechanical.

### Git Strategy

Use `git mv` for all file moves so that `git log --follow` and `git blame` continue to work. Commit Phase 3 as a **single atomic commit** to keep the rename mapping unambiguous in history.

### Move Map

| Current path | Target path | Notes |
| --- | --- | --- |
| `ui/MainWindow.*` | `app/MainWindow.*` | |
| `ui/UIState.h` | `shell/UIState.h` | |
| `ui/StatusBar.*` | `shell/StatusBar.*` | |
| `ui/ThemeBus.*` | `shell/ThemeBus.*` | |
| `ui/GtkControlExecutor.*` | `common/GtkControlExecutor.*` | |
| `ui/SvgTemplate.h` | `common/SvgTemplate.h` | |
| `ui/TrackPageGraph.*` | `track/TrackPageGraph.*` | |
| `ui/TrackViewPage.*` | `track/TrackViewPage.*` | |
| `ui/TrackListAdapter.*` | `track/TrackListAdapter.*` | |
| `ui/TrackListModel.*` | `track/TrackListModel.*` | |
| `ui/TrackPresentation.*` | `track/TrackPresentation.*` | Also contains `TrackColumnLayoutModel` |
| `ui/TrackRow.*` | `track/TrackRow.*` | |
| `ui/TrackRowDataProvider.*` | `track/TrackRowDataProvider.*` | |
| `ui/ListSidebarController.*` | `list/ListSidebarController.*` | |
| `ui/ListRow.*` | `list/ListRow.*` | |
| `ui/ListTreeNode.*` | `list/ListTreeNode.*` | |
| `ui/SmartListDialog.*` | `list/SmartListDialog.*` | |
| `ui/QueryExpressionBox.*` | `list/QueryExpressionBox.*` | |
| `ui/PlaybackController.*` | `playback/PlaybackController.*` | |
| `ui/AobusSoul.*` | `playback/AobusSoul.*` | |
| `ui/AobusSoulWindow.*` | `playback/AobusSoulWindow.*` | Full-screen easter egg; re-wire into `OutputButtonComponent` in Phase 6 |
| `ui/VolumeBar.*` | `playback/VolumeBar.*` | |
| `ui/OutputListItems.h` | `playback/OutputListItems.h` | |
| `ui/InspectorSidebar.*` | `inspector/InspectorSidebar.*` | |
| `ui/CoverArtCache.*` | `inspector/CoverArtCache.*` | |
| `ui/CoverArtWidget.*` | `inspector/CoverArtWidget.*` | |
| `ui/TagEditController.*` | `tag/TagEditController.*` | |
| `ui/TagPopover.*` | `tag/TagPopover.*` | |
| `ui/TagEditor.*` | `tag/TagEditor.*` | |
| `ui/ImportExportCoordinator.*` | `library_io/ImportExportCoordinator.*` | |
| `ui/ImportProgressDialog.*` | `library_io/ImportProgressDialog.*` | |
| `service/PlaylistExporter.*` | `library_io/PlaylistExporter.*` | Primary collaborator is `ImportExportCoordinator` |

`layout/LayoutConstants.h` stays in `layout/` because it is consumed by layout components.

### Include Strategy

For this mechanical phase, prefer updating includes to explicit feature-relative paths rather than relying on a broad include directory.

Examples:

```cpp
#include "track/TrackPageGraph.h"
#include "list/ListSidebarController.h"
#include "playback/AobusSoul.h"
#include "inspector/CoverArtCache.h"
#include "tag/TagEditController.h"
#include "library_io/ImportExportCoordinator.h"
```

Keep `target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")` so root-relative includes work.

After all includes are explicit, remove these broad include dirs:

```cmake
target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ui")
target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/service")
target_include_directories(aobus-gtk-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/layout")
```

The `layout/` include directory can be removed because layout-internal files already use local-relative includes, and external consumers will use `layout/...` root-relative paths.

### Required Double Checks

```bash
# Verify no includes point to old paths
rg -n '#include "[^"]+"|#include <[^>]+>' app/linux-gtk
rg -n 'ui/' app/linux-gtk/CMakeLists.txt app/linux-gtk app/runtime include
rg -n 'service/' app/linux-gtk/CMakeLists.txt
rg -n 'target_include_directories\(aobus-gtk-lib PUBLIC "\$\{CMAKE_CURRENT_SOURCE_DIR\}/ui"' app/linux-gtk/CMakeLists.txt

# Quick include-breakage check (faster than full build)
nix-shell --run "cmake --build /tmp/build/debug --target aobus-gtk-lib --parallel 2>&1" | grep "fatal error:"
```

Expected:

- No includes point to deleted or moved files.
- No `ui/` or `service/` sources remain in `CMakeLists.txt`.
- No `fatal error:` lines from the include-breakage check.

### Verification

```bash
./build.sh debug
```

## Phase 3b: Layout Internal Reorganization

Split `layout/` into sub-directories to match the target structure. This is separated from Phase 3 because it reorganizes an already-structured directory rather than evacuating the flat `ui/` bucket.

### Move Map

| Current path | Target path |
| --- | --- |
| `layout/LayoutDocument.*` | `layout/document/LayoutDocument.*` |
| `layout/LayoutNode.h` | `layout/document/LayoutNode.h` |
| `layout/LayoutYaml.h` | `layout/document/LayoutYaml.h` |
| `layout/ILayoutComponent.h` | `layout/runtime/ILayoutComponent.h` |
| `layout/ComponentContext.h` | `layout/runtime/ComponentContext.h` |
| `layout/ComponentRegistry.*` | `layout/runtime/ComponentRegistry.*` |
| `layout/LayoutHost.*` | `layout/runtime/LayoutHost.*` |
| `layout/LayoutRuntime.*` | `layout/runtime/LayoutRuntime.*` |

`layout/LayoutConstants.h` stays at the `layout/` root; it is a leaf dependency used by both `document/` and `components/`.

`layout/components/` and `layout/editor/` are already in sub-directories and do not move.

### Include Updates

Update all layout-internal includes to use the new sub-directory paths:

```cpp
#include "layout/document/LayoutDocument.h"
#include "layout/runtime/ComponentContext.h"
#include "layout/runtime/ComponentRegistry.h"
```

### Verification

```bash
./build.sh debug
```

## Phase 4: Rename Misleading Classes

Do this after folders are stable. Rename one class at a time so compiler errors stay local.

### Recommended Renames

| Current name | New name | Reason |
| --- | --- | --- |
| `TrackPageGraph` | `TrackPageManager` | It manages `Gtk::Stack` page lifecycle, not a graph. |
| `PlaybackController` | `PlaybackSequenceController` | It builds and advances playback sequences from visible track order. |
| `TrackRowDataProvider` | `TrackRowCache` | It caches and creates `TrackRow` objects. |
| `TrackRow` | `TrackRowObject` | It is a `Glib::Object` model item, not a row widget. Keep the singular `Track` prefix. |
| `ListRow` | `ListRowObject` | It is a `Glib::Object` model item. Keep the singular `List` prefix. |
| `ListTreeNode` | `ListTreeItem` | It is a GTK tree-list item object. |
| `InspectorSidebar` | `TrackInspectorPanel` | It displays focused track details, not a generic sidebar. |

### Rename Procedure

For each rename:

1. Rename file(s).
2. Rename class and constructor/destructor.
3. Update includes.
4. Update CMake source list.
5. Update component context fields if applicable.
6. Build `aobus-gtk-lib` before moving to the next rename.

### Required Double Checks

For each old name:

```bash
rg -n "\bOldName\b" app/linux-gtk app/runtime include doc/design doc/plan
```

Expected:

- No code references remain.
- Documentation references are either updated or intentionally retained as historical notes.

## Phase 5: Split `MainWindow` Responsibilities

After cleanup and file moves, split `MainWindow` into a small window class and a controller/composition root.

### New Classes

#### `app::MainWindow`

Responsibilities:

- Inherit `Gtk::ApplicationWindow`.
- Own or expose the root child assignment.
- Provide minimal GTK window APIs used by controllers.

Should not:

- Create feature controllers.
- Register all menu actions.
- Own mutation subscriptions.
- Save/load track column layout.
- Rebuild list pages.

#### `app::WindowController`

Responsibilities:

- Own feature controllers and caches.
- Initialize session.
- Register audio providers.
- Subscribe to runtime mutation events.
- Save/load window and track view state.
- Coordinate rebuilds after imports/list mutations.

#### `app::MenuController`

Responsibilities:

- Build menu model.
- Register window actions.
- Dispatch menu actions to import/export/layout controllers.

#### `shell::ShellLayoutController`

Responsibilities:

- Own `ComponentRegistry`, `ComponentContext`, `LayoutHost`, and active `LayoutDocument`.
- Load/save `linuxGtkLayout`.
- Open layout editor.
- Manage edit-mode callbacks.

#### `app::WindowContext`

Responsibilities:

- Bundle window-scope dependencies that must be shared between controllers.
- Use singular domain fields such as `track`, `list`, and `tag` when exposing feature contexts.
- Avoid a plural `WindowServices` name unless the class really represents a collection of runtime `*Service` objects.

### Interim Ownership Map

Before starting the split, document the **current** ownership of every pointer in `ComponentContext` as an interim ownership map. This map determines which new controller owns which dependency and is the input for Phase 7's `LayoutDependencies` design. Without it, Phase 5 and Phase 7 risk contradictory ownership decisions.

Capture at minimum:

| Dependency | Current owner | Phase 5 owner |
| --- | --- | --- |
| `TrackRowDataProvider` | `MainWindow` | `WindowController` |
| `CoverArtCache` | `MainWindow` | `WindowController` |
| `PlaybackController` | `MainWindow` | `WindowController` |
| `TagEditController` | `MainWindow` | `WindowController` |
| `ImportExportCoordinator` | `MainWindow` | `WindowController` |
| `TrackPageGraph` | `MainWindow` | `WindowController` |
| `TrackColumnLayoutModel` | `MainWindow` | `WindowController` |
| `ListSidebarController` | `MainWindow` | `WindowController` |
| `StatusBar` | `MainWindow` | `ShellLayoutController` |
| `ComponentRegistry` | `MainWindow` | `ShellLayoutController` |
| `ComponentContext` | `MainWindow` | `ShellLayoutController` |
| `LayoutHost` | `MainWindow` | `ShellLayoutController` |
| `MenuModel` | `MainWindow` | `MenuController` |

Update this table during Phase 5 implementation as ownership decisions are finalized.

### Required Double Checks

```bash
rg -n "setupMenu|setupLayoutHost|openLayoutEditor|saveSession|loadSession|rebuildListPages|updateImportProgress" app/linux-gtk/app app/linux-gtk/shell app/linux-gtk
```

Expected after completion:

- `MainWindow` no longer contains these orchestration functions.
- Equivalent behavior exists in controller classes.

### Verification

```bash
./build.sh debug
```

Manual smoke test if a graphical session is available:

1. Start `aobus-gtk`.
2. Verify menu appears.
3. Verify playback row appears.
4. Verify library sidebar appears.
5. Verify track table appears after session initialization.
6. Toggle inspector handle.
7. Open layout editor and cancel; layout should revert.
8. Import/export actions should still open dialogs.

## Phase 6: Split Large Feature Classes

This phase reduces class complexity without changing visible behavior.

### Recommended Priority Order

Execute domain splits in this order (easiest-first to build confidence, hardest-last when the pattern is established):

1. **Library IO** — cleanest facade split, smallest surface area.
2. **List** — moderate complexity, well-bounded model/view separation.
3. **Playback** — medium complexity but already partially componentized via layout components.
4. **Track** — highest complexity (`TrackViewPage` is ~1400 lines), highest value. Do last when the split pattern is proven.

### Track Domain

Split `TrackViewPage` gradually:

1. `TrackColumnController`
   - column creation metadata
   - visibility/order/width sync
   - shared `TrackColumnLayoutModel` updates

2. `TrackFilterController`
   - debounce timer
   - filter expression resolution
   - filter status subscription and UI state

3. `TrackSelectionController`
   - selected ID extraction
   - primary selection
   - selected duration
   - runtime view selection sync

Keep `TrackViewPage` as the composed page until the smaller controllers are stable.

### List Domain

Split `ListSidebarController` gradually:

1. `ListTreeModelBuilder`
   - construct `Gio::ListStore<ListTreeNode>` from library lists.

2. `ListSidebarPanel`
   - own `Gtk::ListView`, context popover, factories, and selection model.

3. Keep `ListSidebarController`
   - list CRUD
   - smart-list dialog orchestration
   - workspace navigation callback

### Playback

Current playback controls live inside `layout/components/PlaybackComponents.cpp`. Extract reusable widgets/controllers without changing component types:

1. `TransportControls` or `PlayPauseButton`/`StopButton` widgets.
2. `SeekControl` for seek scale and display-synchronized updates.
3. `TimeLabel` if kept separate from `SeekControl`.
4. `OutputSelector` for output button/popover/list.
5. `QualityIndicator` for visual quality display.

Then make layout components thin wrappers over these reusable classes.

Additionally, restore the `AobusSoulWindow` easter egg in `OutputButtonComponent`:

- Port the right-click long-press gesture (1-second hold) from the old `PlaybackBar::setupLayout()` to `OutputButtonComponent`.
- On long-press: construct or show `AobusSoulWindow`, set its transient parent, and call `present()`.
- Keep the existing left-click → popover behavior unchanged.

### Library IO

Split `ImportExportCoordinator`:

1. `LibraryOpenController`
2. `LibraryImportController`
3. `LibraryExportController`
4. Keep `ImportExportCoordinator` as a facade while callers migrate.

## Phase 7: Replace `ComponentContext` With Domain Dependencies

`ComponentContext` currently contains many optional pointers. After `MainWindow` is split, replace it with grouped dependencies.

### Target Shape

```cpp
struct LayoutDependencies final
{
  ComponentRegistry const& registry;
  ao::rt::AppSession& session;
  Gtk::Window& parentWindow;

  ShellUiContext& shell;
  TrackUiContext& track;
  ListUiContext& list;
  PlaybackUiContext& playback;
  InspectorUiContext& inspector;
  TagUiContext& tag;
  LibraryIoContext& libraryIo;

  LayoutEditContext* edit = nullptr;
};
```

### Migration Rules

- Components should depend on the smallest domain service they need.
- Avoid exposing raw feature controllers unless the component truly wraps that controller.
- Keep error-placeholder behavior when required dependencies are unavailable.

## Global Verification Checklist

Run these checks after every phase that changes files, includes, or CMake.

### Source Reference Checks

```bash
rg -n "\bPlaybackBar\b|\bOutputMenuModel\b|\bTagPromptDialog\b|\bGtkMainThreadDispatcher\b" app/linux-gtk app/runtime include
rg -n "\bAobusSoulWindow\b" app/linux-gtk app/runtime include  # must still be present in playback/
rg -n "#include \"ui/|#include <ui/|ui/.*\.cpp" app/linux-gtk app/runtime include CMakeLists.txt
rg -n "TODO|FIXME|Error: .* missing" app/linux-gtk/layout/components app/linux-gtk/app app/linux-gtk/ui
```

### Conformance Check

Run after every phase to catch style regressions introduced by moved or edited files:

```bash
./build.sh debug --tidy
```

### Build and Test Checks

Preferred final check:

```bash
./build.sh debug
```

Targeted iteration check:

```bash
nix-shell --run "cmake --build /tmp/build/debug --target aobus-gtk-lib --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test"
```

### Manual GTK Smoke Test

If a graphical session is available:

1. Launch the GTK app.
2. Confirm default layout loads.
3. Confirm menu actions are present.
4. Confirm playback controls update with playback service state.
5. Confirm output selector opens and can select a device/profile.
6. Confirm library sidebar selection navigates views.
7. Confirm smart-list create/edit/delete still works.
8. Confirm track table displays, filters, groups, selects, and activates tracks.
9. Confirm tag popover/editor still applies tag changes.
10. Confirm inspector cover art, metadata, audio properties, and tag editor still update from focused selection.
11. Confirm import/export dialogs still open and status/progress updates still appear.
12. Confirm layout editor preview/cancel/apply behavior still works.
13. Confirm right-click-hold-1s on output button triggers full-screen `AobusSoulWindow` easter egg (click or Escape to dismiss).

## Documentation Updates

Because this reorganization changes maintainers' mental model but should not intentionally change user-facing behavior, update design docs as follows:

1. Update `doc/design/free-layout/02-semantic-components.md` after deleting `PlaybackBar`, `OutputMenuModel`, or moving playback controls. Note that `AobusSoulWindow` is preserved and re-wired into `OutputButtonComponent`.
2. Update `doc/design/free-layout/03-mainwindow-migration.md` after splitting `MainWindow`.
3. Update `doc/design/THREADING.md` if `GtkMainThreadDispatcher` is deleted or `IMainThreadDispatcher` is no longer part of the GTK threading model.
4. Update `doc/design/linux-gtk-ui-design.md` after folder/namespace reorganization is complete.

## Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Deleting a class that is only loaded indirectly by layout YAML. | Check registered component types and default/custom layout nodes. Only delete classes not referenced by registry factories. |
| Breaking user-saved layout configs. | Preserve all existing component type strings during reorg. Do not rename component types in the same phase as class/file moves. |
| Include path ambiguity after file moves. | Prefer root-relative includes and keep only `${CMAKE_CURRENT_SOURCE_DIR}` as public include root. |
| Runtime callback lifetime regressions. | Keep ownership unchanged during mechanical moves; split ownership only in later phases. |
| Playback UI regressions after deleting `PlaybackBar`. | Verify every old visible control has an equivalent `playback.*` component in the default layout. |
| Easter egg regression after deleting `PlaybackBar`. | `AobusSoulWindow` must be re-wired into `OutputButtonComponent` (right-click-hold-1s) in Phase 6. Verify long-press behavior in smoke test. |
| Threading regression after deleting `GtkMainThreadDispatcher`. | Confirm `GtkControlExecutor` remains the only executor passed to `AppSession` and runtime services. |

## Git Commit Strategy

Each phase should produce well-scoped commits that are easy to review and bisect.

| Phase | Commits | Rationale |
| --- | --- | --- |
| Phase 1 | 1 commit | Small, self-contained member/include removal. |
| Phase 2 | 1 commit per deletion sub-section (2.1–2.4), or 1 combined commit | Independent deletions; separate commits help isolate regressions. |
| Phase 3 | 1 atomic commit | Single `git mv` batch preserves `git blame` and `git log --follow`. |
| Phase 3b | 1 atomic commit | Same reasoning as Phase 3. |
| Phase 4 | 1 commit per rename | Compiler errors stay local to one rename. |
| Phase 5 | 1 commit per new class extraction | Incremental split; each commit should build and pass tests. |
| Phase 6 | 1 commit per domain split | Independent sub-domains. |
| Phase 7 | 1 commit | Single structural refactor of `ComponentContext`. |

## Recommended First Implementation Batch

The first batch should be intentionally small:

1. Remove stale `MainWindow` members and includes.
2. Delete `OutputMenuModel`.
3. Delete `TagPromptDialog`.
4. Build.

Only after that succeeds:

5. Delete `PlaybackBar`.
6. Build.
7. Remove `GtkMainThreadDispatcher` if no code references remain.
8. Build and run full tests.

This order keeps independent cleanup separate from playback and threading cleanup, making failures easier to isolate.
