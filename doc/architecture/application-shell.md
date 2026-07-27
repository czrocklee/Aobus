---
id: architecture.application-shell
type: architecture
status: current
domain: application-shell
summary: Defines ownership and lifetime boundaries for the declarative shell, actions, component construction, layout sessions, and platform adaptation.
---
# Application shell architecture

## Scope

This document owns the structural graph of Aobus desktop application shells.
It covers the GTK declarative layout session, catalogs, registries and factories, action activation and Gio export, per-build dependency wiring, component runtime state, editor rebuilds, shortcuts, and teardown. It also owns the WinUI Modern/Classic shell boundary, stable session ownership, responsive policy, and native adapter placement.

It does not own the semantic state rendered by a track, playback, workspace, status, or resource component.
It does not make the GTK declarative layout system a cross-frontend contract: WinUI uses fixed native shells and TUI builds its terminal shell independently.

The subject qualifies as an application-system architecture because it has an independent document, catalog, construction, action, persistence, rebuild, and lifetime graph that presentation alone cannot explain without absorbing GTK shell orchestration.

## System context

The [architecture landscape](README.md) classifies the application shell as an application system.
The [system architecture](system-overview.md) places reusable layout values and policies in UIModel and concrete widget construction in the GTK frontend.

```text
UIModel
  LayoutDocument -> bounded preparation -> PreparedLayout
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

WinUI follows a parallel native composition:

```text
App owns DispatcherQueueExecutor + LibrarySession + MainWindow
  -> shared DesktopShellPolicy + settings/theme schemas
  -> MainWindow retains Modern and Classic XAML trees
     -> WindowsUiCoordinator owns the LibrarySession callback boundary
        -> TrackListController + cover-art presenters + theme coordinator
     -> playback leaf controls own native event tokens + UIModel bindings
     -> mode switch changes visibility and title-bar ownership
  -> SMTC, picker, AppWindow, and WASAPI adapters remain Windows-local
```

## Responsibilities

### Platform-neutral shell language

UIModel owns `LayoutDocument`, `LayoutNode`, `LayoutValue`, shell-owned preparation limits, bounded template expansion, `PreparedLayout`, node-id validation, component and action descriptor types, the platform-neutral component/action catalogs, component-state documents and promotion policy, `ShellLayoutSessionModel`, and keymap values and policy.

These values describe structure, stable command identity, validation metadata, and UI-local state without naming GTK widget classes, GDK key symbols, or runtime storage objects beyond narrow managed-state adapters.

### GTK shell owner

`MainWindow` owns one `ShellLayoutController` for the complete window lifetime.
The controller is the current GTK shell composition owner: it selects and loads a preset, owns the active layout session, component and action registries, runtime component state, layout host, editor workflow, stores, action export, and the borrowed collaborators used by factories.

This is a broad current responsibility set, not a general-purpose runtime facade.
Customized-layout load/save/remove operations and candidate preparation return typed results; component-state persistence retains its narrower optional/Boolean contract.
The underlying grouped store provides fail-closed one-shot replacement without generic commit receipts or a blocked-store recovery mode.

### WinUI shell owner

`App` owns the dispatcher, `LibrarySession`, and `MainWindow` in destruction-safe order. `LibrarySession` remains alive across shell switches and owns active-library replacement, playback-command binding, and Windows settings. `MainWindow` owns both XAML trees for its lifetime and switches only presentation visibility and title-bar mode.

`WindowsUiCoordinator` is the one window-scoped owner of the exclusive `LibrarySession` callback registration. It owns the stable track-list, cover-art, and theme collaborators and retires that registration before releasing them. `WinUiDependencies` is a construction-scoped aggregate of borrowed references: a consumer unpacks only the narrow references it retains and requests a fresh aggregate after a library or playback runtime replacement.

Playback buttons, Soul transport, seek, time, and volume adapters are leaf controls. Each leaf owns every XAML event token it registers and its corresponding UIModel ViewModel, starts every bind with an idempotent unbind, and reconciles the native control from the ViewModel's initial callback. Modern and Classic use separate native adapters even when they render the same semantic command.

Shared UIModel values own responsive breakpoints, Soul constants and animation gating, bounded row and artwork caching, and strict Windows settings/theme schemas. WinUI owns XAML controls, HWND and `AppWindow` adaptation, `DispatcherQueue` delivery, FolderPicker, SMTC, and WASAPI provider registration.

### Component and action registries

The GTK `ComponentRegistry` pairs each platform-neutral `LayoutComponentDescriptor` with one GTK `ComponentFactory`.
Its embedded UIModel catalog is the editor and validator authority for registered type metadata.

The GTK `ActionRegistry` pairs each `LayoutActionDescriptor` with one handler and optional availability provider.
It remains the live command authority; layout nodes, keyboard maps, and Gio actions refer to stable action ids instead of duplicating command behavior.

### Layout construction

`prepareLayout()` meters the authored document, performs bounded template expansion, and produces the only value accepted by `LayoutRuntime` and `LayoutHost`.
`LayoutRuntime` recursively requests GTK components from that prepared effective root.
`LayoutHost` owns the active component tree and exposes a staged `prepare`/`commit` boundary for load, editor preview/save, reset, and panel-size promotion.

Factories receive one `LayoutBuildContext` assembled for the build.
They borrow `AppRuntime`, the parent window, registries, shell-lifetime runtime state, an explicit candidate-state view, and `GtkUiDependencies`; they do not reach through a global service locator.

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
- WinUI does not parse or adapt the GTK layout document. Its fixed native shells consume shared semantic policy without introducing a second runtime authority.
- `WinUiDependencies` may be passed during construction or binding but cannot be retained as a service locator. Replaceable runtime and command references are reacquired after the corresponding `LibrarySession` callback.
- A WinUI leaf adapter owns only its native controls, native event registrations, and narrow UIModel bindings. It does not reach through `MainWindow` for ordinary playback state or commands.
- Presentation owners define the semantic values a component renders; shell owns placement, construction, binding, and component lifetime.

## Data and control flow

### Startup and rebuild

```text
global application preference selects classic or modern
  -> worker bounded-loads customized layout or preserves/rejects it and selects built-in
  -> worker prepares authored/effective layout
  -> worker loads matching component-state document or empty state
  -> callback executor builds a detached GTK candidate against candidate state
  -> stateful node ids are diagnosed
  -> controller installs ShellLayoutSessionModel and runtime state
  -> LayoutHost commits the prepared tree
```

### Action route

```text
layout component, menu, shortcut, or Gio action id
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
  -> bounded preview prepare/build without committing layout or runtime component state
  -> Save prepares the active tree before writing modified presets and pruning runtime state
  -> session installs active preset/document/state
  -> host commit

component interaction
  -> state entry keyed by node id
  -> type/version/baseline guard
  -> per-preset runtime-state file
```

Reset removes runtime component state without changing authored YAML.
Panel-size promotion moves eligible splitter size values into authored defaults and retains non-promoted state with a refreshed baseline.

### WinUI runtime replacement

```text
LibrarySession announces library/playback changing
  -> WindowsUiCoordinator and MainWindow unbind observers of the old source
  -> LibrarySession commits the prepared replacement
  -> LibrarySession announces changed
  -> MainWindow requests fresh WinUiDependencies
  -> stable controllers and leaf controls bind and reconcile immediately
```

The changing callback is the last point at which an adapter may safely detach from the retiring source. The changed callback never reuses runtime or command references captured from an earlier dependency aggregate.

## Structural constraints

- One `ShellLayoutController` and one active `LayoutHost` belong to one GTK `MainWindow`.
- `ShellLayoutSessionModel` is the active preset/document authority; editor working copies and preview trees are not authoritative.
- Raw `LayoutDocument` values remain authored/session/persistence values; only `PreparedLayout` may enter GTK construction.
- `LayoutBuildContext` is created for one recursive build and cannot be retained as shell wiring.
- Candidate construction reads preset, component state, edit mode, callbacks, and generation from its explicit build-state view.
- `LayoutRuntimeState` outlives every component that can write component state.
- Stateful component identity is a stable expanded node id, never tree position.
- Anonymous stateful nodes remain usable but non-persistent; duplicate stateful ids are invalid for save.
- Component-state generation prevents an old tree from writing into a newly installed state document.
- A detached tree captures the next generation; commit advances that generation before destroying the retiring tree.
- Authored layouts, component runtime state, global preset selection, and keyboard overrides remain separate persistence classes.
- Unknown component and template references become visible diagnostic components rather than undefined factory calls.
- For WinUI, one main window owns both shell trees. Modern is the default. Switching shell mode cannot replace the active `LibrarySession`, initiate scanning, or stop playback.
- WinUI responsive layout may collapse side regions but cannot hide the track list or playback controls. Classic Soul remains a separate 32 by 32 glyph-free control before ordinary Play/Pause and Stop controls.
- One `WindowsUiCoordinator` owns the `LibrarySession` callback sink for one WinUI main window.
- A WinUI dependency aggregate is bounded to one construction or bind operation and is not stored.
- Each playback leaf owns and revokes its native event token while its XAML element is still alive. Its `bind` operation first performs an idempotent `unbind`.
- Modern Soul transport owns Play/Pause and glyph rendering. Classic Soul owns no transport command or glyph and remains independent from Classic Play/Pause and Stop.

## Failure, cancellation, and lifetime boundaries

Layout load runs on the shared worker pool and returns to the frontend callback executor before installing state or building GTK widgets.
`ShellLayoutController` owns a lifetime scope; teardown cancels outstanding workflows before destroying editor and component trees.

Unknown or recursive templates become error nodes.
Unknown component types become visible layout-error widgets.
Malformed, unsupported, or over-budget customized layouts fall back to the matching built-in document without rewriting the rejected file.
Missing customization is normal absence rather than rejection.
The grouped store preserves the previous document on returned failure.

Document preparation and detached GTK construction happen before active session/tree replacement.
A failed preparation keeps the old generation live.
Strict version dispatch, preparation budgets, and rejected-file preservation form the complete layout-document safety boundary; there is no migration or generic candidate framework.

During teardown the controller clears the host while runtime state, stores, registries, and borrowed dependencies are still alive.
The final clear does not advance the state generation, allowing the current component tree to flush pending state before its owners disappear.
Editor theme and callback tokens are released before the controller's collaborators.

WinUI unregisters XAML, runtime, playback, SMTC, and cover-art observations before its window releases the shared session. `WindowsUiCoordinator::retire()` first clears the session callback registration, then unbinds its owned collaborators. Playback leaf destructors unbind their ViewModels before revoking native event tokens, while the main window's XAML tree is still alive.

Library replacement prepares a candidate runtime while the active runtime remains usable.
The candidate loads an existing canonical database directly, or completes an initial scan when the selected root is new.
The session then unbinds library observers immediately before committing the candidate.
Cancellation or failure destroys only the candidate.

## Implementation map

- [`LayoutDocument`](../../app/include/ao/uimodel/layout/document/LayoutDocument.h), [`LayoutNode`](../../app/include/ao/uimodel/layout/document/LayoutNode.h), and [`LayoutPreparation`](../../app/include/ao/uimodel/layout/document/LayoutPreparation.h) own the platform-neutral document and preparation proof.
- UIModel layout action, component, document, and shell types live under [`app/include/ao/uimodel/layout/`](../../app/include/ao/uimodel/layout/) and [`app/uimodel/layout/`](../../app/uimodel/layout/).
- [`ShellLayoutController`](../../app/linux-gtk/app/ShellLayoutController.h) is the current GTK shell owner.
- [`ComponentRegistry`](../../app/linux-gtk/layout/runtime/ComponentRegistry.h), [`ActionRegistry`](../../app/linux-gtk/layout/runtime/ActionRegistry.h), [`LayoutRuntime`](../../app/linux-gtk/layout/runtime/LayoutRuntime.h), and [`LayoutHost`](../../app/linux-gtk/layout/runtime/LayoutHost.h) own GTK construction and activation.
- [`ShellLayoutStore`](../../app/linux-gtk/app/ShellLayoutStore.h) and [`ShellLayoutComponentStateStore`](../../app/linux-gtk/app/ShellLayoutComponentStateStore.h) own customized layouts and component state.
- [`KeymapModel`](../../app/include/ao/uimodel/input/KeymapModel.h), [`KeymapApplicator.cpp`](../../app/linux-gtk/app/KeymapApplicator.cpp), and [`ShortcutEditorWidget.cpp`](../../app/linux-gtk/preference/ShortcutEditorWidget.cpp) own neutral policy and GTK adaptation.
- [`App`](../../app/windows-winui/App.xaml.h), [`LibrarySession`](../../app/windows-winui/app/LibrarySession.h), and [`MainWindow`](../../app/windows-winui/MainWindow.xaml) own the WinUI shell and stable-session composition; shell, track, and playback code-behind methods are compiled from their matching subsystem directories.
- [`WindowsUiCoordinator`](../../app/windows-winui/app/WindowsUiCoordinator.h) owns the WinUI session callback boundary, while [`WinUiDependencies`](../../app/windows-winui/app/WinUiDependencies.h) is the construction-scoped borrowed dependency aggregate.
- [`PlaybackControls`](../../app/windows-winui/playback/PlaybackControls.h) composes the fixed Modern and Classic playback surfaces. [`TransportButton`](../../app/windows-winui/playback/TransportButton.h), [`SoulTransportButton`](../../app/windows-winui/playback/SoulTransportButton.h), [`OutputDeviceControl`](../../app/windows-winui/playback/OutputDeviceControl.h), [`SeekControl`](../../app/windows-winui/playback/SeekControl.h), [`PlaybackTimeControl`](../../app/windows-winui/playback/PlaybackTimeControl.h), and [`VolumeControl`](../../app/windows-winui/playback/VolumeControl.h) own playback leaf adaptation.
- [`DesktopShellPolicy`](../../app/include/ao/uimodel/layout/shell/DesktopShellPolicy.h) and [`AobusSoulViewModel`](../../app/include/ao/uimodel/playback/soul/AobusSoulViewModel.h) own shared Windows shell and Soul policy.

## Test map

- UIModel layout tests under [`test/unit/uimodel/layout/`](../../test/unit/uimodel/layout/) protect document, bounded preparation, templates, catalogs, actions, component-state, promotion, and session policy.
- GTK layout runtime and component tests under [`test/unit/linux-gtk/layout/`](../../test/unit/linux-gtk/layout/) protect construction, registry injection, actions, surfaces, editor behavior, and component state.
- [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) protects shell ownership by the window.
- Keymap tests under [`test/unit/uimodel/input/`](../../test/unit/uimodel/input/) and [`ShortcutEditorWidgetTest.cpp`](../../test/unit/linux-gtk/preference/ShortcutEditorWidgetTest.cpp) protect neutral and GTK shortcut boundaries.
- The UIModel organization guardrail in [`AssertUimodelOrganization.cmake`](../../cmake/AssertUimodelOrganization.cmake) protects platform-neutral placement.
- Windows UIModel tests under [`test/unit/uimodel/`](../../test/unit/uimodel/) protect breakpoints, persistence, theme fallback, Soul constants, playback ViewModels, and bounded caches. Native `winui` Debug and Release builds protect XAML and Windows adapter composition; the current repository has no WinUI widget-test host.

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
- [Windows desktop shell specification](../spec/shell/windows-desktop.md)
- [Windows desktop state reference](../reference/windows/desktop-state.md)
