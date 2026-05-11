# Phase 1: Layout Model and Runtime

## Goal

Introduce the smallest layout runtime that can build a GTK widget tree from a YAML-backed model. This phase should not migrate the full application yet. It should prove the core architecture with simple containers and a few placeholder or existing semantic components.

## Scope

Add a new layout module under `app/linux-gtk/layout/`:

```text
app/linux-gtk/layout/
  LayoutDocument.h/.cpp
  LayoutNode.h/.cpp
  LayoutRuntime.h/.cpp
  LayoutHost.h/.cpp
  ComponentRegistry.h/.cpp
  ComponentContext.h
  ComponentDescriptor.h
```

The first implementation should support:

- YAML load/save model structs.
- A component registry keyed by node `type`.
- Container factories for `box`, `split`, `tabs`, `scroll`, and `spacer`.
- A thin `LayoutHost` that owns the root component and exposes `Gtk::Widget& widget()`.
- Built-in fallback layout creation in C++ for safe startup.

Containers listed in the overview but not in this list are deferred. `grid`, `stack`, and `overlay` should not be required for Phase 1 validation. `app.workspaceWithInspector` may internally use an existing `Gtk::Stack` during migration, but that does not imply a generic user-facing `stack` container exists yet.

## Runtime Model

Use explicit structs rather than exposing yaml-cpp nodes throughout the UI:

```cpp
namespace ao::gtk::layout
{
  struct LayoutValue final
  {
    // Phase 1 can be a small std::variant of bool, int64, double, string,
    // vector<string>. Add nested maps later only when needed.
  };

  struct LayoutNode final
  {
    std::string id;
    std::string type;
    std::map<std::string, LayoutValue, std::less<>> props;
    std::map<std::string, LayoutValue, std::less<>> layout;
    std::vector<LayoutNode> children;
  };

  struct LayoutDocument final
  {
    std::uint32_t version = 1;
    LayoutNode root;
  };
}
```

The runtime should preserve unknown props during load/save once the model supports it. In Phase 1, it is acceptable for component-specific props to be parsed only by the component that owns them.

Because `LayoutNode` is recursive, do not rely only on `ConfigStore`'s generic boost PFR serialization. Add custom yaml-cpp conversion code for `LayoutValue`, `LayoutNode`, and `LayoutDocument`. The custom conversion should handle recursive `children`, preserve node order, and leave room for unknown-field round-tripping.

The first `LayoutValue` can support scalars and string vectors, but the model should be designed so nested maps can be added without changing the YAML schema. This is needed for future props such as per-side margins:

```yaml
layout:
  margin:
    top: 4
    bottom: 2
```

## YAML Schema

The persistent schema is YAML:

```yaml
version: 1
root:
  id: root
  type: box
  props:
    orientation: vertical
    spacing: 6
  children:
    - type: playback.playPauseButton
    - type: playback.stopButton
    - type: spacer
      layout:
        hexpand: true
    - type: status.messageLabel
```

Field meanings:

- `version`: layout schema version, not application version.
- `root`: root node.
- `id`: optional stable node identity. Auto-generate if absent.
- `type`: registry key.
- `props`: component-owned typed properties.
- `layout`: parent/container-owned placement properties.
- `children`: ordered child nodes.

## Component Interfaces

Keep the interface intentionally small:

```cpp
class ILayoutComponent
{
public:
  virtual ~ILayoutComponent() = default;
  virtual Gtk::Widget& widget() = 0;
};

struct ComponentContext final
{
  ComponentRegistry const& registry;
  ao::rt::AppSession& session;
  Gtk::Window& parentWindow;
  TrackRowDataProvider* rowDataProvider = nullptr;
  CoverArtCache* coverArtCache = nullptr;
};

using ComponentFactory = std::unique_ptr<ILayoutComponent> (*)(ComponentContext&, LayoutNode const&);
```

This is the Phase 1 subset of the canonical context described in the overview. Later phases add shared playback projections and coordinator pointers without changing the factory shape.

Avoid `updateProps()` in Phase 1. Rebuild the affected subtree when a layout changes. Live prop mutation can be introduced during editor work.

## Container Ownership

GTK ownership needs to be explicit. Each component should own its direct GTK root widget as a member or `std::unique_ptr`. Container components should own child `ILayoutComponent` instances in a vector so their GTK widgets stay alive.

Example structure:

```cpp
class BoxComponent final : public ILayoutComponent
{
public:
  Gtk::Widget& widget() override { return _box; }

private:
  Gtk::Box _box;
  std::vector<std::unique_ptr<ILayoutComponent>> _children;
};
```

Do not use `Gtk::make_managed` for root widgets that are owned by layout components unless ownership and lifetime are very clear. Existing GTK code often uses managed children; the layout runtime should prefer C++ ownership for component roots.

## Container Factories

All container factories should apply common widget properties before parent attachment:

- `visible`
- `hexpand`
- `vexpand`
- `halign`
- `valign`
- scalar `margin`
- `minWidth` / `minHeight`
- `cssClasses`

`cssClasses` maps to GTK widget CSS classes. Prefer adding classes individually through the gtkmm API available in the project version rather than replacing existing style classes accidentally.

### `box`

Props:

```yaml
props:
  orientation: horizontal # horizontal | vertical
  spacing: 6
  homogeneous: false
```

Child layout props:

```yaml
layout:
  hexpand: true
  vexpand: false
  halign: fill
  valign: center
  margin: 4
```

### `split`

Use `Gtk::Paned`, matching the current `MainWindow` split implementation.

Props:

```yaml
props:
  orientation: horizontal
  position: 300
  resizeStart: true
  shrinkStart: false
  resizeEnd: true
  shrinkEnd: false
```

Phase 1 should accept exactly two children. Invalid child count should produce a fallback error widget.

Mapping:

- `orientation` -> `Gtk::Paned::set_orientation()`
- first child -> `Gtk::Paned::set_start_child()`
- second child -> `Gtk::Paned::set_end_child()`
- `position` -> `Gtk::Paned::set_position()`
- `resizeStart` -> `Gtk::Paned::set_resize_start_child()`
- `shrinkStart` -> `Gtk::Paned::set_shrink_start_child()`
- `resizeEnd` -> `Gtk::Paned::set_resize_end_child()`
- `shrinkEnd` -> `Gtk::Paned::set_shrink_end_child()`

Set resize/shrink flags before or immediately after child attachment, then set `position` once both children exist. Persisting user-resized paned positions belongs to Phase 3.

### `tabs`

Use `Gtk::Stack` plus `Gtk::StackSwitcher` or `Gtk::Notebook`. Prefer `Gtk::Stack` if it fits existing GTK4 conventions.

Child layout props:

```yaml
layout:
  title: All Tracks
  icon: view-list-symbolic
```

Mapping for a `Gtk::Stack` implementation:

- child node `id` or generated id -> `Gtk::StackPage::set_name()` / add child name
- child layout `title` -> `Gtk::StackPage::set_title()`
- child layout `icon` -> `Gtk::StackPage::set_icon_name()` when supported by the project gtkmm version
- active tab state -> stored separately from the static layout unless the user explicitly edits the default active page

### `scroll`

Use `Gtk::ScrolledWindow` and accept exactly one child. Invalid child count should produce a fallback error widget.

Props:

```yaml
props:
  hscrollPolicy: automatic # never | automatic | always | external
  vscrollPolicy: automatic # never | automatic | always | external
  minContentWidth: 360
  minContentHeight: 240
  propagateNaturalWidth: false
  propagateNaturalHeight: false
```

Mapping:

- `hscrollPolicy` / `vscrollPolicy` -> `Gtk::ScrolledWindow::set_policy()`
- child -> `Gtk::ScrolledWindow::set_child()`
- `minContentWidth` / `minContentHeight` -> `set_min_content_width()` / `set_min_content_height()`
- `propagateNaturalWidth` / `propagateNaturalHeight` -> `set_propagate_natural_width()` / `set_propagate_natural_height()` when supported by the project gtkmm version

### `spacer`

Use an empty `Gtk::Box` or custom `Gtk::Widget` with expansion controlled by layout props.

## Error Handling

The runtime must never crash on a bad layout file.

Required fallback behavior:

- Unknown component type -> visible error component with the missing type name.
- Invalid prop value -> use default and log a warning.
- Invalid container child count -> show error component inside that container.
- YAML parse failure -> load built-in default layout.

## Rebuild Limitation

Phase 1 should rebuild affected subtrees rather than mutate live props. This is simple and safe, but it loses transient widget state such as scroll positions, text selection, expander state, and in-progress edits. Later editor phases should preserve selected high-value state explicitly or implement targeted mutation for components where rebuild loss is user-visible.

## Tests

Add focused tests for model and registry behavior:

- YAML round-trip preserves node types, ids, props, layout props, and child order.
- Custom yaml-cpp conversion handles recursive `children` without relying on boost PFR recursion.
- Unknown component creation returns an error component.
- `split` rejects child counts other than two without crashing.
- Built-in default document validates.

If GTK widget construction is difficult in headless tests, keep Phase 1 tests mostly at the model/registry validation layer and add a small runtime smoke test only if the existing test environment supports GTK initialization.

## Completion Criteria

- `LayoutDocument` can be loaded from and saved to YAML.
- `LayoutRuntime` can build a GTK tree from the built-in default document.
- `ComponentRegistry` can instantiate at least `box`, `split`, `scroll`, `spacer`, and one semantic placeholder component.
- Bad layout files fall back safely.
- Documentation and tests use YAML examples only.
