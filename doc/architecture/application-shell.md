---
id: architecture.application-shell
type: architecture
status: current
domain: application-shell
summary: Defines ownership and lifetime boundaries for the declarative shell, actions, component construction, layout sessions, and platform adaptation.
---
# Application shell architecture

## Scope

This document owns the structural graph that turns a platform-neutral layout document and action/component metadata into Aobus's live GTK application shell.
It covers layout-session authority, catalogs, GTK registries and factories, action activation and Gio export, per-build dependency wiring, component runtime state, editor rebuilds, shortcuts, and teardown.

It does not own the semantic state rendered by a track, playback, workspace, status, or resource component.
It also does not make the GTK declarative layout system a cross-frontend contract: TUI currently builds its terminal shell independently.

The subject qualifies as an application-system architecture because it has an independent document, catalog, construction, action, persistence, rebuild, and lifetime graph that presentation alone cannot explain without absorbing GTK shell orchestration.

## System context

The [architecture landscape](README.md) classifies the application shell as an application system.
The [system architecture](system-overview.md) places reusable layout values and policies in UIModel and concrete widget construction in the GTK frontend.

```text
UIModel
  LayoutDocument + template expansion
  LayoutComponentCatalog + LayoutActionCatalog
  ShellLayoutSessionModel + component-state policy + keymap model
           |
           v
GTK MainWindow owns ShellLayoutController
  ComponentRegistry: descriptor -> GTK factory
  ActionRegistry: descriptor -> handler/state provider
  LayoutHost -> LayoutRuntime -> component tree
  GioActionBridge + keymap accelerator adapter
  ShellLayoutStore + component-state store
```

The [presentation architecture](presentation.md) owns the broader runtime/UIModel/frontend split.
This document refines the shell-specific composition inside that split and does not move widget types into UIModel.

## Responsibilities

### Platform-neutral shell language

UIModel owns `LayoutDocument`, `LayoutNode`, `LayoutValue`, template expansion, node-id validation, component and action descriptor types, the platform-neutral component/action catalogs, component-state documents and promotion policy, `ShellLayoutSessionModel`, and keymap values and policy.

These values describe structure, stable command identity, validation metadata, and UI-local state without naming GTK widget classes, GDK key symbols, or runtime storage objects beyond narrow managed-state adapters.

### GTK shell owner

`MainWindow` owns one `ShellLayoutController` for the complete window lifetime.
The controller is the current GTK shell composition owner: it selects and loads a preset, owns the active layout session, component and action registries, runtime component state, layout host, editor workflow, stores, action export, and the borrowed collaborators used by factories.

This is a broad current responsibility set, not a general-purpose runtime facade.
Persistence calls in several editor and panel-size paths are log-only or void, so a rebuild can proceed without a coherent durable commit receipt; [RFC 0015](../rfc/0015-fail-closed-config-store.md) owns the proposed fail-closed store boundary.

### Component and action registries

The GTK `ComponentRegistry` pairs each platform-neutral `LayoutComponentDescriptor` with one GTK `ComponentFactory`.
Its embedded UIModel catalog is the editor and validator authority for registered type metadata.

The GTK `ActionRegistry` pairs each `LayoutActionDescriptor` with one handler and optional availability provider.
It remains the live command authority; layout nodes, keyboard maps, notification actions, and Gio actions refer to stable action ids instead of duplicating command behavior.

### Layout construction

`LayoutRuntime` expands document templates and recursively requests GTK components from the registry.
`LayoutHost` owns the active component tree and replaces it as a unit on load, editor preview, save, reset, or panel-size promotion.

Factories receive one `LayoutBuildContext` assembled for the build.
They borrow `AppRuntime`, the parent window, registries, shell-lifetime runtime state, and an explicit `GtkUiDependencies` view; they do not reach through a global service locator.

### State and input adaptation

Component runtime state is separate from authored layout structure.
Stateful GTK components resolve state by stable node id, component type, state version, and a baseline hash, then persist interaction state through the shell-lifetime store.

The keymap model binds neutral chords to action ids.
GTK translates those chords to native accelerator syntax and applies eligible window actions; the shortcut editor remains a GTK view over UIModel policy.

## Boundaries and dependency direction

- UIModel shell types may depend on stable runtime values and managed-state mechanisms but never on GTK, GDK, Gio, or frontend-local classes.
- GTK layout registries depend on UIModel descriptors and concrete GTK factories; UIModel catalogs do not depend back on those registries.
- Component factories may consume explicit runtime/UIModel collaborators from `LayoutBuildContext`; runtime services never depend on layout components.
- Layout documents carry stable component types, properties, action ids, and semantic CSS class values, not C++ factory names or widget pointers.
- The action registry owns activation and availability; a component binding or keymap is a reference, not a parallel handler.
- Gio export and shortcut application stop at the GTK boundary.
- TUI may reuse action, keymap, or layout values deliberately, but the current GTK document cannot be described as the TUI shell authority.
- Presentation owners define the semantic values a component renders; shell owns placement, construction, binding, and component lifetime.

## Data and control flow

### Startup and rebuild

```text
global application preference selects classic or modern
  -> worker loads customized layout or built-in preset
  -> worker loads matching component-state document or empty state
  -> callback executor installs ShellLayoutSessionModel state
  -> stateful node ids are diagnosed
  -> controller assembles a fresh LayoutBuildContext
  -> LayoutHost replaces the component tree
  -> template expansion + registry factories create GTK widgets
```

### Action route

```text
layout component, menu, shortcut, or notification action id
  -> ActionRegistry validation and availability
  -> activation context with runtime, window, anchor, and component id
  -> registered handler
  -> runtime/UIModel command or GTK-local surface
```

Gio export includes only actions for which the shell can provide the required anchor/menu context.
The bridge refreshes enabled state from the live registry.

### Editor and component state

```text
editor working document
  -> preview rebuild without committing layout or runtime component state
  -> Save writes modified presets and prunes matching runtime state
  -> session installs active preset/document/state
  -> host rebuild

component interaction
  -> state entry keyed by node id
  -> type/version/baseline guard
  -> per-preset runtime-state file
```

Reset removes runtime component state without changing authored YAML.
Panel-size promotion moves eligible splitter size values into authored defaults and retains non-promoted state with a refreshed baseline.

## Structural constraints

- One `ShellLayoutController` and one active `LayoutHost` belong to one GTK `MainWindow`.
- `ShellLayoutSessionModel` is the active preset/document authority; editor working copies and preview trees are not authoritative.
- `LayoutBuildContext` is created for one recursive build and cannot be retained as shell wiring.
- `LayoutRuntimeState` outlives every component that can write component state.
- Stateful component identity is a stable expanded node id, never tree position.
- Anonymous stateful nodes remain usable but non-persistent; duplicate stateful ids are invalid for save.
- Component-state generation prevents an old tree from writing into a newly installed state document.
- Authored layouts, component runtime state, global preset selection, and keyboard overrides remain separate persistence classes.
- Unknown component and template references become visible diagnostic components rather than undefined factory calls.

## Failure, cancellation, and lifetime boundaries

Layout load runs on the shared worker pool and returns to the frontend callback executor before installing state or building GTK widgets.
`ShellLayoutController` owns a lifetime scope; teardown cancels outstanding workflows before destroying editor and component trees.

Unknown or recursive templates become error nodes.
Unknown component types become visible layout-error widgets.
Malformed custom layout or component-state files currently fall back or log according to their store; several save/remove operations do not return a complete commit outcome to the shell coordinator.
Those current best-effort boundaries are documented by the shell specifications and persistence architecture, while RFC 0015 proposes stronger transaction semantics.
Authored layout version values are currently decoded without a supported-version gate, and file/model/template/widget construction has no shared product budget; [RFC 0025](../rfc/0025-bounded-shell-layout-documents.md) proposes one strict bounded candidate pipeline.

During teardown the controller clears the host while runtime state, stores, registries, and borrowed dependencies are still alive.
The final clear does not advance the state generation, allowing the current component tree to flush pending state before its owners disappear.
Editor theme and callback tokens are released before the controller's collaborators.

## Implementation map

- [`LayoutDocument`](../../app/include/ao/uimodel/layout/document/LayoutDocument.h), [`LayoutNode`](../../app/include/ao/uimodel/layout/document/LayoutNode.h), and [`LayoutTemplateExpansion.cpp`](../../app/uimodel/layout/document/LayoutTemplateExpansion.cpp) own the platform-neutral document.
- UIModel layout action, component, document, and shell types live under [`app/include/ao/uimodel/layout/`](../../app/include/ao/uimodel/layout/) and [`app/uimodel/layout/`](../../app/uimodel/layout/).
- [`ShellLayoutController`](../../app/linux-gtk/app/ShellLayoutController.h) is the current GTK shell owner.
- [`ComponentRegistry`](../../app/linux-gtk/layout/runtime/ComponentRegistry.h), [`ActionRegistry`](../../app/linux-gtk/layout/runtime/ActionRegistry.h), [`LayoutRuntime`](../../app/linux-gtk/layout/runtime/LayoutRuntime.h), and [`LayoutHost`](../../app/linux-gtk/layout/runtime/LayoutHost.h) own GTK construction and activation.
- [`ShellLayoutStore`](../../app/linux-gtk/app/ShellLayoutStore.h) and [`ShellLayoutComponentStateStore`](../../app/linux-gtk/app/ShellLayoutComponentStateStore.h) own customized layouts and component state.
- [`KeymapModel`](../../app/include/ao/uimodel/input/KeymapModel.h), [`KeymapApplicator.cpp`](../../app/linux-gtk/app/KeymapApplicator.cpp), and [`ShortcutEditorWidget.cpp`](../../app/linux-gtk/preferences/ShortcutEditorWidget.cpp) own neutral policy and GTK adaptation.

## Test map

- UIModel layout tests under [`test/unit/uimodel/layout/`](../../test/unit/uimodel/layout/) protect document, template, catalog, action, component-state, promotion, and session policy.
- GTK layout runtime and component tests under [`test/unit/linux-gtk/layout/`](../../test/unit/linux-gtk/layout/) protect construction, registry injection, actions, surfaces, editor behavior, and component state.
- [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) protects shell ownership by the window.
- Keymap tests under [`test/unit/uimodel/input/`](../../test/unit/uimodel/input/) and [`ShortcutEditorWidgetTest.cpp`](../../test/unit/linux-gtk/preferences/ShortcutEditorWidgetTest.cpp) protect neutral and GTK shortcut boundaries.
- The UIModel organization guardrail in [`AssertUimodelOrganization.cmake`](../../cmake/AssertUimodelOrganization.cmake) protects platform-neutral placement.

## Related documents

- [Architecture landscape](README.md)
- [System architecture](system-overview.md)
- [Presentation architecture](presentation.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Shell layout lifecycle specification](../spec/shell/layout-lifecycle.md)
- [Keyboard shortcut specification](../spec/shell/keyboard-shortcut.md)
- [Shell layout-adaptation specification](../spec/shell/layout-adaptation.md)
- [Layout document reference](../reference/shell/layout-document.md)
- [Layout component-state reference](../reference/shell/layout-state.md)
- [Layout catalog and action reference](../reference/shell/layout-catalog.md)
- [Keyboard map reference](../reference/shell/keymap.md)
- [RFC 0025: bounded shell layout documents](../rfc/0025-bounded-shell-layout-documents.md)
