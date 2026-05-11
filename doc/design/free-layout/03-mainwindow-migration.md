# Phase 3: MainWindow Migration and YAML Persistence

## Goal

Replace the hard-coded top-level GTK layout with a layout document loaded through `ConfigStore`, while preserving the current default user experience.

This phase turns `MainWindow` into a thin shell and makes the current application layout a default YAML-backed document.

## Current MainWindow Responsibilities

`MainWindow` currently owns and wires:

- `TrackRowDataProvider`
- `CoverArtCache`
- `CoverArtWidget`
- `ListSidebarController`
- `ImportExportCoordinator`
- `Gtk::Stack`
- menu bar and actions
- `TrackPageGraph`
- `TagEditController`
- `GtkMainThreadDispatcher`
- `PlaybackBar`
- `PlaybackController`
- runtime subscriptions for mutations/import/list changes
- `StatusBar`
- `InspectorSidebar` and revealer handle

The migration should preserve `MainWindow` as the owner of long-lived systems but move visual placement into `LayoutHost`.

## Target MainWindow Shape

```cpp
class MainWindow final : public Gtk::ApplicationWindow
{
public:
  explicit MainWindow(ao::rt::AppSession& session, std::shared_ptr<ao::rt::ConfigStore> configStore);

private:
  void setupSystems();
  void setupMenu();
  void setupLayoutHost();
  void initializeSession();

  ao::rt::AppSession& _session;
  std::shared_ptr<ao::rt::ConfigStore> _configStore;
  layout::LayoutHost _layoutHost;
  layout::ComponentRegistry _componentRegistry;
  layout::ComponentContext _componentContext;

  // Existing managers remain here until each becomes a component/system.
};
```

`MainWindow` should keep ownership of objects whose lifetime is larger than any single layout subtree. Examples: `TrackRowDataProvider`, `CoverArtCache`, `PlaybackController`, and import/export coordinator.

## YAML Persistence

Use `ConfigStore` and YAML. Phase 3 should store one active layout document under a dedicated group:

```yaml
linuxGtkLayout:
  version: 1
  root:
    type: box
    props:
      orientation: vertical
    children: []
```

Multi-layout storage is future work for the editor/template phase. A later format can add `activeLayoutId` and a `layouts` map, but Phase 3 should not implement both shapes simultaneously.

Avoid JSON fallback files. If a layout cannot be parsed, keep the invalid YAML file in place and load the built-in default layout for the current session.

## Built-in Default Layout

The default layout should match the current visual structure:

```yaml
version: 1
root:
  id: app-root
  type: box
  props:
    orientation: vertical
  children:
    - type: app.menuBar
    - id: playback-row
      type: box
      props:
        orientation: horizontal
        spacing: 6
      children:
        - type: playback.outputButton
        - type: playback.playPauseButton
        - type: playback.stopButton
        - type: playback.seekSlider
          layout:
            hexpand: true
        - type: playback.timeLabel
        - type: playback.volumeControl
    - id: main-paned
      type: split
      props:
        orientation: horizontal
        position: 330
        shrinkStart: false
        shrinkEnd: false
      layout:
        vexpand: true
      children:
        - id: left-sidebar
          type: box
          props:
            orientation: vertical
          children:
            - type: library.listTree
              layout:
                vexpand: true
            - type: inspector.coverArt
              layout:
                minHeight: 50
        - id: workspace-with-inspector
          type: app.workspaceWithInspector
    - type: status.defaultBar
```

`app.workspaceWithInspector` can initially be a semantic composite because the current inspector handle/revealer/stack behavior is not yet generic. Later phases can express it using `overlay`, `revealer`, and individual inspector components.

This adapter is transitional and should not become the permanent layout abstraction. If Phase 3 ships with `app.workspaceWithInspector`, users will not yet be able to freely place every track workspace and inspector subcomponent. That limitation is acceptable for the migration phase only; Phase 4/5 work should decompose it into reusable nodes such as `tracks.workspaceStack`, `inspector.trackDetails`, `revealer`, and `overlay`.

## Component Context Wiring

The migration uses the canonical context shape from the overview. By Phase 3 the relevant fields are:

```cpp
struct ComponentContext final
{
  ao::rt::AppSession& session;
  Gtk::Window& parentWindow;
  TrackRowDataProvider* rowDataProvider = nullptr;
  CoverArtCache* coverArtCache = nullptr;
  PlaybackUiProjection* playbackUi = nullptr;
  PlaybackController* playbackController = nullptr;
  TagEditController* tagEditController = nullptr;
  ImportExportCoordinator* importExportCoordinator = nullptr;
};
```

This is intentionally explicit. Do not use a service locator in Phase 3.

## Action Placement

GTK `Gio::ActionMap` registration can remain in `MainWindow` and existing controllers during this phase. Layout components that need actions should either:

1. call runtime services directly, or
2. call an existing coordinator through `ComponentContext`.

Do not introduce a generic `action: playback.stop` layer until there is a clear need for user-created primitive buttons.

Keyboard accelerators must be registered outside layout components. Layout changes should never unregister core shortcuts. The recommended split is:

- application/window action registration: stable owner, usually `MainWindow` or an action registrar
- visible controls: semantic layout components that call the same runtime/coordinator methods
- editor action picker: future optional UI, not required until primitive user-defined buttons exist

## CSS Provider Lifecycle

Some existing widgets install global CSS providers. Dynamic layout creation makes per-instance global providers risky because components can be created, destroyed, and recreated repeatedly.

Rules for migration:

- Prefer widget CSS classes from YAML `cssClasses` and shared application-level providers.
- If a semantic component needs CSS, register its provider idempotently at application scope or ensure it removes the provider when destroyed.
- Do not install a new global provider for every component instance.
- Avoid using component destruction order to manage CSS that other live components may still need.

## Top-Level Component Adapters

Some existing objects are not easy to make freely instantiable immediately. Add temporary semantic adapters:

- `app.menuBar`: wraps the existing `Gtk::PopoverMenuBar`.
- `app.workspaceWithInspector`: wraps the existing stack, inspector handle, and revealer.
- `status.defaultBar`: wraps existing `StatusBar`.
- `library.listTree`: wraps existing `ListSidebarController`.

These adapters allow the top-level layout to become YAML-driven before every nested widget is decomposed.

## State Preservation

Persist both layout document and runtime workspace state:

- Layout document controls visual placement.
- `WorkspaceService::saveSession()` continues to persist open views and focused view.
- Existing `WindowState` and track column layout state should remain independent.

Do not mix open track view state into the free layout YAML. A `tracks.table` node can declare `view: workspace.focused`, but the list of open views remains runtime workspace state.

## Startup Ordering

Build the layout host before restoring workspace views that emit focus or view lifecycle events. A safe startup sequence is:

1. Construct long-lived systems and component registry.
2. Build `LayoutHost` from the active or built-in layout document.
3. Create `TrackRowDataProvider` and load library row data during `initializeSession()`.
4. Update `ComponentContext` or system adapters with initialized providers.
5. Rebuild/listen components that require initialized data, such as track workspace components.
6. Call `WorkspaceService::restoreSession()`.
7. If no views were restored, navigate to the default all-tracks view.

Components that receive an event before their data dependency is ready should ignore it or render a clear pending/error state rather than dereferencing null context pointers.

## YAML Serialization Detail

`ConfigStore` can store the final layout group, but `LayoutDocument` needs custom yaml-cpp conversion because recursive `LayoutNode::children` cannot be safely handled by the generic boost PFR helper alone. The layout module should provide explicit load/save helpers and then pass the resulting YAML node or serializable wrapper through `ConfigStore`.

## Tests

- Loading missing layout group uses built-in default.
- Loading invalid YAML falls back to default and logs an error.
- Saving a modified split position updates the YAML layout group.
- Current default layout instantiates all required top-level components.
- Existing session restore still opens the same focused track view.
- Keyboard shortcuts still work when their corresponding visible button is omitted from the layout.
- CSS provider registration does not multiply when rebuilding the same layout repeatedly.

## Completion Criteria

- `MainWindow::setupLayout()` no longer hard-codes the full visual hierarchy.
- The default UI is generated from a layout document.
- User layout YAML can be loaded from `ConfigStore`.
- Invalid layout YAML does not prevent startup.
- Existing playback, sidebar navigation, track views, inspector, import/export, and status behavior remain intact.
