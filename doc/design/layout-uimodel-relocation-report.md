# Layout and Action System Relocation Report

## Executive Summary

The HEAD commit introduces a flexible action system and extends the GTK layout runtime. The investigation confirms that a substantial part of this work is not intrinsically coupled to GTK and should move from `app/linux-gtk/layout` into the platform-neutral UI-model layer.

The right split is to move the reusable **model/catalog** layer to `ao_app_uimodel`, while keeping the GTK **renderer/binder** layer in `app/linux-gtk`.

```text
╭──────────────────────────────╮
│ ao_app_uimodel               │
│ layout document/schema       │
│ action metadata + registry   │
│ component descriptor catalog │
│ action validation            │
│ template expansion           │
╰──────────────┬───────────────╯
               │ consumed by
     ╭─────────┴─────────╮
     ▼                   ▼
╭─────────────╮     ╭──────────────╮
│ linux-gtk   │     │ windows-winui │
│ GTK widgets │     │ WinUI views   │
│ Gio bridge  │     │ WinUI bridge  │
│ GTK editor  │     │ WinUI editor  │
╰─────────────╯     ╰──────────────╯
```

This gives the upcoming WinUI3 shell reusable layout schema, YAML format, action IDs/capabilities, validation rules, component/property descriptor vocabulary, and template-expansion behavior without forcing it to depend on GTK widgets, Gio, Glib resources, or GTK-specific UI services.

## Current Dependency Boundary

`ao_app_uimodel` is already described in `app/CMakeLists.txt` as the platform-neutral UI-model library for Linux GTK, WinUI3, and tests. The new action/layout files, however, currently live in `aobus-gtk-lib` through `app/linux-gtk/CMakeLists.txt`:

- `layout/document/LayoutDocument.cpp`
- `layout/document/LayoutNode.cpp`
- `layout/runtime/LayoutRuntime.cpp`
- `layout/runtime/ComponentRegistry.cpp`
- `layout/runtime/ActionRegistry.cpp`
- `layout/runtime/GioActionBridge.cpp`
- `layout/runtime/ActionValidator.cpp`
- `layout/runtime/ActionBinder.cpp`
- `layout/runtime/LayoutHost.cpp`
- concrete GTK component and editor files

Because `aobus-gtk-lib` already links against `ao_app_uimodel`, the desired dependency direction is already available: GTK can consume shared UI-model layout types, but shared UI-model code must not depend on GTK.

The relocation should therefore be a split, not a blanket directory move.

## Components That Should Move to `app/uimodel`

Recommended shared namespace:

```cpp
namespace ao::uimodel::layout
```

Recommended shared file roots:

```text
app/include/ao/uimodel/layout/
app/uimodel/layout/
test/unit/uimodel/layout/
```

### Layout Document Model

Move:

- `LayoutNode.h`
- `LayoutNode.cpp`
- the platform-neutral parts of `LayoutDocument.h`
- the YAML serialization/deserialization parts of `LayoutDocument.cpp`
- `LayoutYaml.h`

Recommended target files:

```text
app/include/ao/uimodel/layout/LayoutNode.h
app/include/ao/uimodel/layout/LayoutDocument.h
app/include/ao/uimodel/layout/LayoutYaml.h

app/uimodel/layout/LayoutNode.cpp
app/uimodel/layout/LayoutDocument.cpp
```

The following model pieces are reusable:

- `LayoutValue`
- `LayoutNode`
- `LayoutDocument`
- YAML traits for `LayoutValue`, `LayoutNode`, and `LayoutDocument`
- `loadLayout(rt::ConfigStore&, ...)`
- `saveLayout(rt::ConfigStore&, ...)`

These are plain layout data structures and serialization helpers. They use the standard library, `ao::rt::yaml`, and `ConfigStore`, not GTK.

#### Required split: built-in preset loading

Do not move the current built-in preset loader as-is. It uses GTK/Glib resource APIs:

- `Gio::Resource::lookup_data_global(...)`
- `Glib::Error`
- `gsize`

Keep this logic in GTK, preferably under a clearer name:

```text
app/linux-gtk/layout/document/GtkLayoutPresets.h
app/linux-gtk/layout/document/GtkLayoutPresets.cpp
```

Recommended GTK-owned APIs:

```cpp
LayoutDocument createDefaultGtkLayout();
LayoutDocument createBuiltInGtkLayout(LayoutPresetId presetId);
std::map<std::string, LayoutNode, std::less<>> getBuiltInGtkTemplates();
```

If the current names are retained for compatibility, keep them in `ao::gtk::layout` rather than the shared namespace.

### Action Model and Registry

Move most of `ActionRegistry.h` and `ActionRegistry.cpp` after removing GTK from the activation context.

Recommended target files:

```text
app/include/ao/uimodel/layout/ActionRegistry.h
app/uimodel/layout/ActionRegistry.cpp
```

Move these shared types:

- `ActionDescriptor`
- `ActionState`
- `ActionCapability`
- `ActionCapabilities`
- `ActionSlot`
- `ActionBindingProperty`
- `ActionBindingContext`
- `ActionActivationResult`
- `ActionActivationOutcome`
- `ActionRegistry`

The registry logic itself is platform-neutral: it registers descriptors, rejects duplicates, finds descriptors, checks capabilities against binding context, queries state, and dispatches handlers.

#### Required cleanup: `ActionActivationContext`

Current blocker:

```cpp
struct ActionActivationContext final
{
  rt::AppRuntime& runtime;
  Gtk::Window& parentWindow;
  Gtk::Widget& anchorWidget;
  std::string componentId;
};
```

`Gtk::Window&` and `Gtk::Widget&` prevent the registry from moving to `ao_app_uimodel`.

Recommended shared shape:

```cpp
struct ActionActivationContext final
{
  rt::AppRuntime& runtime;
  std::string componentId;
  // Optional: platform-neutral or opaque frontend-owned presentation payload.
};
```

The exact payload should stay minimal. Avoid introducing GTK, Gio, WinUI, HWND, XAML, or other platform handles into `ao::uimodel::layout`.

GTK-specific action handlers that need an anchor, parent window, popover, or menu should live in `app/linux-gtk` and use a GTK-side adaptor or extension context.

### Component Metadata Catalog

Do not move the current `ComponentRegistry` wholesale. It mixes portable metadata with GTK factory/rendering logic.

Move the descriptor half into a new shared catalog:

```text
app/include/ao/uimodel/layout/ComponentCatalog.h
app/uimodel/layout/ComponentCatalog.cpp
```

Shared types:

- `PropertyKind`
- `PropertyDescriptor`
- `ComponentDescriptor`

Shared catalog API:

```cpp
class ComponentCatalog final
{
public:
  void registerComponentDescriptor(ComponentDescriptor descriptor);
  std::vector<ComponentDescriptor> const& descriptors() const;
  std::optional<ComponentDescriptor> descriptor(std::string_view type) const;
};
```

The GTK `ComponentRegistry` should remain in `app/linux-gtk/layout/runtime`, but become a renderer/factory registry. It can either own a `ComponentCatalog` or delegate descriptor lookup to one.

Why split:

- Portable: property kinds, labels, enum values, component categories, child limits, action-binding metadata.
- GTK-specific: `ComponentFactory`, `ILayoutComponent`, `LayoutContext`, `create(...)`, and fallback `Gtk::Label` error component.

This split lets the layout editor and future WinUI tools use the same component/property schema without linking GTK.

### Action Validation

Move `ActionValidator.h` and `ActionValidator.cpp` after the `ComponentCatalog` split.

Recommended target files:

```text
app/include/ao/uimodel/layout/ActionValidator.h
app/uimodel/layout/ActionValidator.cpp
```

The shared validator should depend on:

- `LayoutDocument`
- `LayoutNode`
- `ActionRegistry`
- `ComponentCatalog`

It should not depend on the GTK factory `ComponentRegistry`.

#### Required cleanup: hardcoded binding assumptions

The current validator constructs this context for every action property:

```cpp
ActionBindingContext{
  .slot = propDesc.optActionBinding->slot,
  .hasAnchor = true,
  .hasFocusedView = true,
  .componentType = node.type
};
```

That is too GTK/button-specific for shared validation. The shared layer should make anchor and focus availability explicit.

Recommended extension:

```cpp
struct ActionBindingProperty final
{
  ActionSlot slot = ActionSlot::PrimaryClick;
  bool providesAnchor = false;
  bool providesFocusedView = false;
};
```

GTK button-like descriptors can set `providesAnchor = true`; shortcut/global bindings can leave it false. WinUI can describe its own capabilities accurately.

### Template Expansion

Do not move `LayoutRuntime` as-is, because it builds GTK widgets. Extract the pure template-expansion algorithm.

Recommended target files:

```text
app/include/ao/uimodel/layout/LayoutTemplateExpander.h
app/uimodel/layout/LayoutTemplateExpander.cpp
```

Suggested API:

```cpp
LayoutNode expandTemplates(LayoutDocument const& doc);
```

or:

```cpp
class LayoutTemplateExpander final
{
public:
  static LayoutNode expand(LayoutDocument const& doc);
};
```

Move the current pure behavior:

- `template` node resolution
- missing `templateId` error node
- unknown template error node
- recursive template loop detection
- template node ID override
- layout property override
- prop override except `templateId`
- child append behavior

GTK `LayoutRuntime::build(...)` should then do:

1. expand templates through `ao::uimodel::layout`
2. pass the expanded root to GTK `ComponentRegistry::create(...)`

WinUI can use the same expander and render the expanded tree with a WinUI registry.

## Components That Should Stay in `app/linux-gtk`

### Definitely GTK-Specific Files

Keep:

```text
app/linux-gtk/layout/runtime/ActionBinder.h
app/linux-gtk/layout/runtime/ActionBinder.cpp
app/linux-gtk/layout/runtime/GioActionBridge.h
app/linux-gtk/layout/runtime/GioActionBridge.cpp
app/linux-gtk/layout/runtime/LayoutContext.h
app/linux-gtk/layout/runtime/LayoutHost.h
app/linux-gtk/layout/runtime/LayoutHost.cpp
app/linux-gtk/layout/runtime/ILayoutComponent.h
app/linux-gtk/layout/runtime/LayoutRuntime.h
app/linux-gtk/layout/runtime/LayoutRuntime.cpp
app/linux-gtk/layout/components/*
app/linux-gtk/layout/editor/LayoutEditorDialog.h
app/linux-gtk/layout/editor/LayoutEditorDialog.cpp
app/linux-gtk/app/ShellLayoutController.h
app/linux-gtk/app/ShellLayoutController.cpp
```

Reasons:

- `ActionBinder` binds action properties to GTK callbacks and receives `Gtk::Widget&` anchors.
- `GioActionBridge` is explicitly a GIO action-map adapter.
- `LayoutContext` contains `Gtk::Window`, `Gio::MenuModel`, and GTK service pointers.
- `ILayoutComponent` exposes `Gtk::Widget&`.
- `LayoutHost` owns/renders GTK layout components.
- concrete components construct GTK widgets.
- `LayoutEditorDialog` is a GTK UI; it should consume shared model/catalog types but remain a GTK view.
- `ShellLayoutController` owns GTK action implementations, Gio export, GTK editor launch, popovers, and window presentation.

### GTK Component Registry

Keep a GTK factory registry, but consider renaming it to clarify scope:

```text
app/linux-gtk/layout/runtime/GtkComponentRegistry.h
app/linux-gtk/layout/runtime/GtkComponentRegistry.cpp
```

This is optional. If the namespace remains `ao::gtk::layout`, the current `ComponentRegistry` name is still acceptable after the shared `ComponentCatalog` split.

### GTK Layout Runtime

Keep GTK widget construction in `LayoutRuntime`, optionally renamed later:

```text
app/linux-gtk/layout/runtime/GtkLayoutRuntime.h
app/linux-gtk/layout/runtime/GtkLayoutRuntime.cpp
```

Again, renaming is optional. The important part is extracting shared template expansion.

### GTK Layout Presets

Move the GResource-backed built-in preset loading into GTK-owned files:

```text
app/linux-gtk/layout/document/GtkLayoutPresets.h
app/linux-gtk/layout/document/GtkLayoutPresets.cpp
```

This keeps `giomm/resource.h`, `glib.h`, and `glibmm/error.h` out of `ao_app_uimodel`.

## Naming and Folder Organization

Recommended final organization:

```text
app/include/ao/uimodel/layout/
  ActionRegistry.h
  ActionValidator.h
  ComponentCatalog.h
  LayoutDocument.h
  LayoutNode.h
  LayoutTemplateExpander.h
  LayoutYaml.h

app/uimodel/layout/
  ActionRegistry.cpp
  ActionValidator.cpp
  ComponentCatalog.cpp
  LayoutDocument.cpp
  LayoutNode.cpp
  LayoutTemplateExpander.cpp

app/linux-gtk/layout/document/
  GtkLayoutPresets.h
  GtkLayoutPresets.cpp

app/linux-gtk/layout/runtime/
  ActionBinder.h
  ActionBinder.cpp
  ComponentRegistry.h          # GTK factory registry, or GtkComponentRegistry.h
  ComponentRegistry.cpp
  GioActionBridge.h
  GioActionBridge.cpp
  ILayoutComponent.h
  LayoutContext.h
  LayoutHost.h
  LayoutHost.cpp
  LayoutRuntime.h              # GTK renderer runtime, or GtkLayoutRuntime.h
  LayoutRuntime.cpp
  UiToggleManager.h
  UiToggleManager.cpp
```

Namespace policy:

- shared model/catalog/action layer: `ao::uimodel::layout`
- GTK renderer/editor layer: `ao::gtk::layout`
- GTK editor view: `ao::gtk::layout::editor`

Avoid `ao::layout` for now. This layout system is application UI model, not core domain logic.

## CMake Changes

Add moved files to `ao_app_uimodel` in `app/CMakeLists.txt`, for example:

```cmake
add_library(ao_app_uimodel STATIC
    uimodel/list/ListActionPolicy.cpp
    # ...existing files...
    uimodel/layout/LayoutNode.cpp
    uimodel/layout/LayoutDocument.cpp
    uimodel/layout/ActionRegistry.cpp
    uimodel/layout/ComponentCatalog.cpp
    uimodel/layout/ActionValidator.cpp
    uimodel/layout/LayoutTemplateExpander.cpp
)
```

Remove portable files from `aobus-gtk-lib` in `app/linux-gtk/CMakeLists.txt`:

```text
layout/document/LayoutDocument.cpp
layout/document/LayoutNode.cpp
layout/runtime/ActionRegistry.cpp
layout/runtime/ActionValidator.cpp
```

Keep or add GTK-specific files there:

```text
layout/document/GtkLayoutPresets.cpp
layout/runtime/LayoutRuntime.cpp
layout/runtime/ComponentRegistry.cpp
layout/runtime/GioActionBridge.cpp
layout/runtime/ActionBinder.cpp
layout/runtime/LayoutHost.cpp
layout/components/*.cpp
layout/editor/LayoutEditorDialog.cpp
```

Guardrail: after this move, `ao_app_uimodel` must not include or link:

- GTKMM
- GDKMM
- GioMM
- GlibMM
- GTK/Glib resource APIs
- WinUI or Windows UI handles

It can continue depending on `ao_app_runtime`, `ao`, and RapidYAML-related runtime helpers.

## Test Reorganization

### Move Portable Tests to `ao_test`

Move these from `test/unit/linux-gtk/layout/...` to `test/unit/uimodel/layout/...`:

```text
test/unit/linux-gtk/layout/runtime/ActionRegistryTest.cpp
  -> test/unit/uimodel/layout/ActionRegistryTest.cpp

test/unit/linux-gtk/layout/runtime/ActionValidatorTest.cpp
  -> test/unit/uimodel/layout/ActionValidatorTest.cpp
```

`ActionRegistryTest` currently creates GTK objects only because `ActionActivationContext` requires `Gtk::Window` and `Gtk::Widget`. After context cleanup, the test should become platform-neutral and run in `ao_test`.

`ActionValidatorTest` is already nearly portable. It should switch from GTK `ComponentRegistry` to shared `ComponentCatalog`.

### Split Layout Model Tests

Split the current GTK-side layout model test into portable and GTK-resource-specific tests.

Move portable sections into:

```text
test/unit/uimodel/layout/LayoutValueTest.cpp
test/unit/uimodel/layout/LayoutNodeTest.cpp
test/unit/uimodel/layout/LayoutDocumentYamlTest.cpp
test/unit/uimodel/layout/LayoutTemplateExpanderTest.cpp
```

Portable coverage should include:

- `LayoutValue` serialization
- `LayoutValue` coercion
- `LayoutNode::getProp`
- `LayoutNode::getLayout`
- YAML encode/decode
- layout prop preservation
- child order preservation
- action ID prop preservation
- missing optional fields
- double parsing
- template expansion behavior

Keep GTK resource/default preset assertions under GTK tests:

```text
test/unit/linux-gtk/layout/document/GtkLayoutPresetTest.cpp
```

This test should cover:

- default GTK preset loads from GResource
- classic/modern GTK layout resources decode
- expected GTK default component tree shape

### Keep GTK Tests in `ao_test_gtk`

Keep:

```text
test/unit/linux-gtk/layout/runtime/ActionBinderTest.cpp
test/unit/linux-gtk/layout/runtime/GioActionBridgeTest.cpp
test/unit/linux-gtk/layout/components/*
test/unit/linux-gtk/layout/editor/*
```

Reasons:

- `ActionBinderTest` depends on GTK widget anchors.
- `GioActionBridgeTest` depends on `Gio::SimpleActionGroup`, GTK application setup, and GTK context providers.
- component tests instantiate GTK widgets.
- editor tests drive a GTK dialog.

### Add New Portable Tests

Add `ComponentCatalogTest`:

- registers descriptors
- replaces or rejects duplicate descriptor types, depending on chosen policy
- looks up descriptor by type
- preserves descriptor order if editor palette order depends on it

Add stronger `ActionValidatorTest` cases:

- unknown action ID produces a diagnostic
- non-string action property produces a diagnostic
- empty and `"none"` action IDs are ignored
- anchored action is rejected when binding metadata does not provide an anchor
- focused-view action is rejected when binding metadata does not provide focused view

Add `LayoutTemplateExpanderTest`:

- normal template expansion
- node ID override
- property override
- layout property override
- child append behavior
- missing `templateId`
- unknown template
- recursive template loop

### CMake Test Changes

Add new/moved portable tests to the `# ui model tests (no platform deps)` section of `test/CMakeLists.txt`.

Remove those files from `ao_test_gtk`.

Keep GTK-specific layout runtime, component, and editor tests in `ao_test_gtk`.

## Recommended Migration Sequence

### Phase 1: Shared Document Model

1. Create `app/include/ao/uimodel/layout` and `app/uimodel/layout`.
2. Move `LayoutNode`, `LayoutValue`, `LayoutDocument`, and YAML traits.
3. Split GTK GResource preset loading into `GtkLayoutPresets`.
4. Update includes and namespaces.
5. Move portable layout model tests to `test/unit/uimodel/layout`.
6. Verify `ao_test` builds and runs without GTK.

This phase is mostly mechanical and low risk.

### Phase 2: Shared Action Registry

1. Refactor `ActionActivationContext` to remove GTK types.
2. Move action descriptors, capabilities, state, binding context, and `ActionRegistry` to `ao::uimodel::layout`.
3. Add/adapt GTK-side activation context plumbing for anchored/menu actions.
4. Move `ActionRegistryTest` to `ao_test`.
5. Keep `ActionBinderTest` and `GioActionBridgeTest` in `ao_test_gtk`.

This is the main design-sensitive phase.

### Phase 3: Shared Component Catalog and Validator

1. Extract `ComponentCatalog` from GTK `ComponentRegistry`.
2. Make GTK `ComponentRegistry` use or mirror the shared catalog.
3. Extend `ActionBindingProperty` with explicit anchor/focus availability.
4. Move `ActionValidator` to `ao::uimodel::layout`.
5. Move and expand `ActionValidatorTest`.

This phase prevents the shared validator from baking in GTK assumptions.

### Phase 4: Shared Template Expansion

1. Extract template expansion from GTK `LayoutRuntime`.
2. Add `LayoutTemplateExpanderTest`.
3. Make GTK `LayoutRuntime::build(...)` consume the shared expander.
4. Leave widget creation in GTK.

This gives WinUI the same template behavior immediately.

## Risks and Guardrails

### Risk: GTK leaks into `ao_app_uimodel`

Guardrails:

- no `Gtk::`, `Gio::`, `Glib::`, GDK, HWND, XAML, or WinUI types in `ao::uimodel::layout`
- no GTK/Gio/Glib headers in `app/include/ao/uimodel/layout`
- `ao_app_uimodel` must not link GTKMM/GioMM/GlibMM

### Risk: validation keeps GTK assumptions

Current validation assumes all action bindings have anchors and focused views. Shared validation must use explicit binding metadata.

Guardrails:

- add `providesAnchor` / `providesFocusedView` or equivalent to action binding metadata
- validate against the active shell catalog, not a global GTK catalog
- test anchored/focused-view rejection in `ao_test`

### Risk: built-in layouts are not actually cross-platform yet

The YAML schema is shareable, but current presets may include GTK-flavored component names, CSS classes, and persisted GTK config keys.

Guardrails:

- keep GTK preset loading GTK-owned for now
- do not make WinUI automatically consume GTK custom layout config keys
- if WinUI should use the same YAML files, define the shared component contract explicitly and implement matching component IDs in both renderers

### Risk: action registration is duplicated later

Some actions are pure runtime commands, while others present GTK UI.

Guardrails:

- first move only the registry and metadata
- later extract shared registration helpers for pure runtime actions if GTK and WinUI start duplicating them
- keep presentation actions frontend-specific

## Final Recommendation

Proceed with the relocation, but as a deliberate split:

- move `LayoutNode`, `LayoutDocument` schema/YAML, `ActionRegistry` metadata/logic, descriptor catalog, action validation, and template expansion into `ao_app_uimodel`
- keep GTK binding, Gio bridge, GTK layout context, GTK components, GTK editor dialog, GTK preset resource loading, and shell controller in `app/linux-gtk`
- reorganize tests so shared behavior runs under `ao_test` and GTK behavior remains under `ao_test_gtk`

The most important precondition is removing GTK from `ActionActivationContext`; the second most important is splitting `ComponentRegistry` into shared descriptor catalog plus GTK factory registry.
