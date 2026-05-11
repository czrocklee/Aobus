# Free Layout System Overview

## Purpose

Aobus should grow from a fixed GTK window into a user-configurable layout host. The goal is similar to foobar2000 layout editing, but with finer granularity: users should be able to place individual Aobus controls such as playback buttons, seek sliders, cover art, track tables, status labels, and library trees in arbitrary container layouts.

The layout system should not expose raw GTK widgets as the normal leaf nodes. A leaf node should usually be an Aobus semantic component, for example `playback.playPauseButton`, not `Gtk::ToggleButton`. The component may internally use `Gtk::Button`, `Gtk::ToggleButton`, `Gtk::Scale`, or custom drawing, but the layout document should describe product-level controls.

## Current Code Findings

The existing GTK frontend has several useful foundations:

- `MainWindow` is currently the composition root. It creates the menu, playback bar, list sidebar, cover art widget, track page graph, inspector, import/export coordinator, and status bar.
- `TrackPageGraph` already behaves like a view/page manager. It watches `WorkspaceService`, creates `TrackViewPage` instances, binds projections, reacts to now-playing changes, and removes closed views.
- Runtime services already use typed events through `ao::rt::Signal` and RAII `ao::rt::Subscription`.
- `PlaybackService`, `ViewService`, `WorkspaceService`, `LibraryMutationService`, and `NotificationService` expose typed state and typed subscriptions.
- `ConfigStore` already persists YAML through yaml-cpp and boost PFR field reflection.

These findings favor a small layout runtime that instantiates semantic components and lets those components use existing runtime services directly. A generic JavaScript-like property binding layer is not required for the first implementation.

## Target Architecture

```diagram
╭─────────────────────────────────────────────╮
│ MainWindow                                  │
│ thin GTK shell                              │
╰─────────────────────┬───────────────────────╯
                      │ owns
╭─────────────────────▼───────────────────────╮
│ LayoutHost                                  │
│ root widget, active layout, edit-mode shell │
╰─────────────────────┬───────────────────────╯
                      │ builds
╭─────────────────────▼───────────────────────╮
│ LayoutRuntime                               │
│ YAML model -> GTK widget tree               │
╰─────────────┬─────────────────┬─────────────╯
              │                 │
╭─────────────▼──────╮  ╭───────▼─────────────╮
│ Container Nodes    │  │ Semantic Components │
│ box, split, tabs   │  │ playback.*, tracks.*│
│ grid, scroll, etc. │  │ library.*, status.* │
╰────────────────────╯  ╰─────────────────────╯
```

## Node Types

### Container Nodes

Container nodes are generic because they define structure rather than product behavior:

- `box`
- `split`
- `tabs`
- `stack`
- `grid`
- `scroll`
- `overlay`
- `spacer`

Later phases can add `absoluteCanvas` and floating panels, but the first implementation should prefer GTK-native structured containers.

### Semantic Leaf Components

Semantic components own their GTK implementation and runtime behavior:

- `playback.outputButton`
- `playback.playPauseButton`
- `playback.stopButton`
- `playback.seekSlider`
- `playback.timeLabel`
- `playback.volumeControl`
- `library.listTree`
- `tracks.table`
- `inspector.coverArt`
- `inspector.trackDetails`
- `status.messageLabel`
- `status.importProgress`

This gives button-level freedom without requiring the layout runtime to understand every GTK signal and property.

### Composite Templates

Composite templates are named layout fragments expanded into containers plus semantic leaves:

- `playback.defaultBar`
- `library.defaultSidebar`
- `workspace.defaultMainArea`
- `app.defaultLayout`

Templates are convenience presets, not special runtime widgets.

## YAML Layout Example

```yaml
version: 1
root:
  id: root
  type: split
  props:
    orientation: horizontal
    position: 300
  children:
    - id: left-pane
      type: box
      props:
        orientation: vertical
        spacing: 6
      children:
        - type: library.listTree
          layout:
            vexpand: true
        - type: inspector.coverArt
          layout:
            minHeight: 120

    - id: right-pane
      type: box
      props:
        orientation: vertical
        spacing: 6
      children:
        - id: custom-playback-row
          type: box
          props:
            orientation: horizontal
            spacing: 4
            cssClasses: [playback-toolbar]
          children:
            - type: playback.outputButton
            - type: playback.previousButton
            - type: playback.playPauseButton
              props:
                size: large
            - type: playback.nextButton
            - type: playback.seekSlider
              layout:
                hexpand: true
            - type: playback.timeLabel
            - type: playback.volumeControl

        - type: tracks.table
          props:
            view: workspace.focused
          layout:
            vexpand: true
```

## Principles

1. **Use YAML for persistent layout documents.** JSON examples should not be used in docs or tests for this system.
2. **Generic containers, semantic leaves.** The layout runtime understands structural containers and common layout props. Product controls own their behavior.
3. **No generic GTK property/signal engine in Phase 1.** Components use `AppSession` and typed runtime events directly.
4. **Default UI is a layout document.** The current fixed `MainWindow::setupLayout()` should eventually be represented as a built-in YAML-equivalent document.
5. **Layout editing is incremental.** Build runtime loading first, migrate existing widgets second, then add visual editing.

## Cross-Cutting Constraints

- **GTK4 details must be explicit.** Container nodes should document the exact GTK implementation they use, including ownership and child attachment behavior.
- **Runtime events stay typed.** If several semantic components need the same high-frequency playback state, use a shared typed projection instead of duplicate timers or a generic string binding layer.
- **Actions and accelerators are layout-independent.** Keyboard shortcuts and application/window actions must remain registered by stable application systems even when a layout omits the visible button for that action.
- **CSS provider lifetime must be controlled.** Components created and destroyed by the layout runtime should prefer CSS classes and shared application-level providers over per-instance global providers. If a component installs a provider, it must define cleanup or idempotent registration behavior.
- **Unknown data preservation matters.** The layout model should preserve unknown component types and unknown YAML fields where practical so the editor can round-trip future-version layouts without data loss.
- **GTK tests need environment support.** Model and registry tests can run headless. Widget construction tests need either a display-capable CI setup, such as Xvfb, or a deliberately mocked/fake widget layer.
- **Accessibility is part of semantic components.** Custom-drawn controls should provide labels, roles, tooltips, and keyboard operation where GTK does not provide them automatically.
- **RTL behavior should be considered early.** GTK structured containers handle much of this automatically, but future freeform coordinates and anchors need explicit RTL semantics.
- **Subscription scale is acceptable but visible.** Many semantic components will hold `ao::rt::Subscription` members. This is fine at expected layout sizes, but shared projections should be preferred for high-frequency or heavily duplicated state.

## Canonical Component Context

Individual phases may introduce fields incrementally, but the intended merged context is:

```cpp
struct ComponentContext final
{
  ComponentRegistry const& registry;
  ao::rt::AppSession& session;
  Gtk::Window& parentWindow;

  ao::gtk::TrackRowDataProvider* rowDataProvider = nullptr;
  ao::gtk::CoverArtCache* coverArtCache = nullptr;
  PlaybackUiProjection* playbackUi = nullptr;

  ao::gtk::PlaybackController* playbackController = nullptr;
  ao::gtk::TagEditController* tagEditController = nullptr;
  ao::gtk::ImportExportCoordinator* importExportCoordinator = nullptr;
  ao::gtk::TrackPageGraph* trackPageGraph = nullptr;
  ao::gtk::TrackColumnLayoutModel* columnLayoutModel = nullptr;
  ao::gtk::StatusBar* statusBar = nullptr;
  Glib::RefPtr<Gio::MenuModel> menuModel;
};
```

Rules:

- `session` and `parentWindow` are always required.
- Pointer fields are optional because some components are available before session initialization or only in later migration phases.
- Components that require an optional dependency must render a clear error or pending component if the pointer is null.
- The context is explicit dependency wiring, not a service locator. Prefer adding a field only when a semantic component genuinely needs a long-lived GTK-side system.

## Planned Phases

- [x] [Phase 1: Layout Model and Runtime](01-layout-runtime.md)
- [x] [Phase 2: Semantic Component Extraction](02-semantic-components.md)
- [x] [Phase 3: MainWindow Migration and YAML Persistence](03-mainwindow-migration.md)
- [Phase 4: Layout Editor](04-layout-editor.md)
- [Phase 5: Freeform Layout and Templates](05-freeform-and-templates.md)
