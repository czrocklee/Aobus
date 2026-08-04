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
It covers the GTK declarative layout session, catalogs, registries and factories, action activation and Gio export, per-build dependency wiring, component runtime state, editor rebuilds, shortcuts, and teardown. It also owns the WinUI Modern/Classic shell boundary, window/session ownership, responsive policy, and native adapter placement.

It does not own the semantic state rendered by a track, playback, workspace, status, or resource component.
It does not make the GTK declarative layout system a cross-frontend contract: the document, catalogs, and build-time state vocabulary are platform-neutral, but component construction is not, so WinUI builds its own presets against its own catalog while TUI builds its terminal shell independently.

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
App owns dispatcher + one LibraryWindowSession
  -> one process owns one LibrarySession + MainWindow for its lifetime
  -> aobus-winui-lib owns ShellStatePolicy + settings/theme schemas
  -> MainWindow owns the XAML frame, its resources, and one layout host
     -> UiCoordinator owns session callbacks and borrowed runtime consumers
        -> TrackListController + cover-art presenters + theme coordinator
     -> ShellBuilder owns actions, menus, pane accessors, runtime state
        -> one live generation built from the preset the resolved mode names
        -> playback leaf controls own native event tokens + UIModel bindings
     -> mode switch replaces the whole generation
  -> SMTC, picker, AppWindow, and WASAPI adapters remain Windows-local
```

## Responsibilities

### Platform-neutral shell language

UIModel owns `LayoutDocument`, `LayoutNode`, `LayoutValue`, shell-owned preparation limits, bounded template expansion, `PreparedLayout`, node-id validation, component and action descriptor types, the platform-neutral component/action catalogs, component-state documents and promotion policy, `ShellLayoutSessionModel`, and keymap values and policy.

It also owns the build-time vocabulary a frontend needs to drive those values: `LayoutSurface` and its descriptor capability mask, the shell-lifetime `LayoutRuntimeState` carrier, the `LayoutBuildStateView` over live or candidate state, the `StatefulComponentState` persistence ritual, and action-slot resolution (`isActionSlotBound`, `boundActionSlots`, `hasBoundActionSlot`).

These values describe structure, stable command identity, validation metadata, and UI-local state without naming GTK widget classes, GDK key symbols, or runtime storage objects beyond narrow managed-state adapters.

### Windows shell composition contracts

[Decision 0004](../decision/0004-adopt-layout-documents-for-winui-shell-composition.md) makes the version 1 layout language the Windows shell composition language while leaving construction with the frontend.
That boundary has two halves, and the split is by who decides rather than by what compiles.
UIModel owns what both shells decide the same way: reading the version 1 common layout fields once, walking a candidate against a catalog and a dialect, the parse-expand-validate step, and the `ShellGenerationSequence` that keeps exactly one view generation live.
The Windows-only `aobus-winui-lib` owns what is this shell's own: its component and action catalogs, the component-to-element mapping, the layout dialect that extends the shared rules with `styleKey` and themed surfaces, `styleKey` lookup planning, the WinUI interpretation of the common fields, and `ShellStatePolicy` for native-window breakpoints and pane modes.
The two built-in preset documents ship under `app/windows-winui/layout/`.
The [Windows layout catalog reference](../reference/windows/layout-catalog.md) owns those exact surfaces.

Both halves name XAML type and resource-scope identities as values rather than as C++/WinRT types, so the pure rules remain testable without constructing a XAML host.
Being portable at the source level is not what makes something shared: a catalog that registers `windows.navigationPane` speaks for one shell however cleanly it compiles, so it lives in `aobus-winui-lib` and is tested only by the native Windows suite.
Sharing the language is not sharing a runtime: there is no cross-frontend build plan or responsive classifier, GTK uses none of these contracts, and Windows keeps native construction, parent placement, controller binding, generation-owned view adapters and the focused-selection projection its components share, `winui::ShellStatePolicy` width boundaries, and `winui::DesktopSettings` pane persistence.

The WinUI window builds its shell from the selected preset. `MainWindow.xaml` keeps the window frame, the single layout host, `RootGrid.Resources`, styles, and compiled `DataTemplate` resources; it composes no shell of its own.

### GTK shell owner

`MainWindow` owns one `ShellLayoutController` for the complete window lifetime.
The controller is the current GTK shell composition owner: it selects and loads a preset, owns the active layout session, component and action registries, runtime component state, layout host, editor workflow, stores, action export, and the borrowed collaborators used by factories.

This is a broad current responsibility set, not a general-purpose runtime facade.
Customized-layout load/save/remove operations and candidate preparation return typed results; component-state persistence retains its narrower optional/Boolean contract.
The underlying grouped store provides fail-closed one-shot replacement without generic commit receipts or a blocked-store recovery mode.

### WinUI shell owner

`App` owns the dispatcher and one `LibraryWindowSession` in destruction-safe order.
The window session owns exactly one `LibrarySession` and `MainWindow`; the session owns exactly one runtime for the process lifetime, and neither owner can retarget or replace it.
Opening another root queues an application-level destructive restart rather than constructing another graph in the process.
Shell mode changes never replace the session.
`MainWindow` owns the XAML frame, its resource scope, the single layout host, and the commands a preset can name; it holds no shell composition of its own.

`ShellBuilder` is the window's shell-lifetime composition owner. It holds the action registry, the menu composition, the `winui::DesktopSettings` pane accessors, the component runtime state, current shell state, retained status message, native-window activity, their change signals, and the layout host. `LayoutHost` only stages and publishes generations; it does not relay state into their components. The builder decides whether the target shell mode requires a new preset generation and commits a changed shell state only after any required candidate publishes. A build that fails leaves the current generation and its committed state live and reports why; a first build that fails leaves the frame showing a minimal layout-error surface, because a shipped document that does not build is an artifact defect rather than a user-recoverable state.

`UiCoordinator` is the one window-scoped owner of the `LibrarySession` status/failure callbacks and the borrowed track-list, resource-byte, and theme collaborators.
It exposes those owned collaborators through narrow accessors and retires their bindings before the window releases its session.
`ShellBuilder` resolves its existing `LibrarySession` into explicit runtime services and a narrow `ShellLibraryAccess` before constructing a per-build `LayoutBuildContext`; generation components never receive `LibrarySession` or `AppRuntime`.
Leaf controls receive the exact runtime or UIModel service their binding needs.

Playback buttons, Soul transport, seek, time, and volume adapters are leaf controls. Each leaf owns every XAML event token it registers and its corresponding UIModel ViewModel, starts every bind with an idempotent unbind, and reconciles the native control from the ViewModel's initial callback. Modern and Classic use separate native adapters even when they render the same semantic command.

A WinUI component observes mutable UI state through a current-value-plus-signal contract. It reads the current value during construction, connects directly to the owning source, and retains the returned `async::Subscription` as a component member. `ShellBuilder` directly owns shell state, native-window activity, transient status, and their signals; `TrackListController` owns track-list changes. Those owners outlive `LayoutHost`, so generation destruction releases every subscription before its source. There is no generic shell-state coordinator, component observer interface, raw observer list, or host fan-out path to synchronize with tree replacement.

`aobus-winui-lib` owns this shell's responsive breakpoints and strict Windows settings and theme schemas, because only this shell uses or persists them.
Shared UIModel values own Soul constants and animation gating, and bounded row and artwork caching.
WinUI also owns XAML controls, HWND and `AppWindow` adaptation, `DispatcherQueue` delivery, FolderPicker, SMTC, and WASAPI provider registration.

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
The context is a GTK aggregate over neutral parts: its surface kind, runtime state, and build-state view are UIModel values, while the parent window, dependency aggregate, detail scope, and timeout scheduler stay GTK-local.

### State and input adaptation

Component runtime state is separate from authored layout structure.
Stateful components resolve state by stable node id, component type, state version, and a baseline hash, then persist interaction state through the shell-lifetime store.
`StatefulComponentState` owns that resolution and the write guards, so a frontend component declares only *what* it persists.
It refuses to write on a tooltip surface, in edit mode, for an anonymous node, without an active preset or store, and once the shell has replaced the state document — the captured generation no longer matches, so a component destructing after a reset, load, or save-defaults cannot pollute the new document.

The keymap model binds neutral chords to action ids.
GTK translates those chords to native accelerator syntax and applies eligible window actions; the shortcut editor remains a GTK view over UIModel policy.

## Boundaries and dependency direction

- UIModel shell types may depend on stable runtime values and managed-state mechanisms but never on GTK, GDK, Gio, or frontend-local classes.
- Build-time state, surface, and action-slot vocabulary lives in UIModel and is usable without a widget toolkit. Whether a second frontend adopts it is a separate decision; hoisting the vocabulary does not by itself make the component system cross-frontend.
- GTK layout registries depend on UIModel descriptors and concrete GTK factories; UIModel catalogs do not depend back on those registries.
- Component factories may consume explicit runtime/UIModel collaborators from `LayoutBuildContext`; runtime services never depend on layout components.
- Layout documents carry stable component types, properties, action ids, and semantic CSS class values, not C++ factory names or widget pointers.
- The action registry owns activation and availability; a component binding or keymap is a reference, not a parallel handler.
- Gio export and shortcut application stop at the GTK boundary.
- TUI may reuse action, keymap, or layout values deliberately, but the current GTK document cannot be described as the TUI shell authority.
- WinUI does not parse or adapt the GTK layout document. It shares the layout language and genuinely cross-frontend semantic policy, while keeping its presets, shell-state policy, and catalog, without introducing a second runtime authority.
- WinUI construction roots pass explicit borrowed services and `ShellLibraryAccess` operations into `LayoutBuildContext`; generation components and leaf adapters cannot accept `LibrarySession`, `AppRuntime`, or a complete window dependency graph.
- Coordinator-owned track-list, resource-byte, and theme collaborators remain valid until the window retires its shell generation and then its coordinator before destroying the owning session.
- A WinUI leaf adapter owns only its native controls, native event registrations, and narrow UIModel bindings. It does not reach through `MainWindow` for ordinary playback state or commands.
- Presentation owners define the semantic values a component renders; shell owns placement, construction, binding, and component lifetime.
- A WinUI component reads an observable source's current value before retaining its scoped subscription. The source owns publication; neither sibling components nor `LayoutHost` relay the change.

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

### WinUI shell-state change

```text
mode + width + inspector request
  -> winui::ShellStatePolicy resolves the target state
  -> build and publish a candidate first when the preset changes
  -> ShellBuilder commits and emits only a changed state
  -> each live component applies the state through its scoped subscription
```

A failed required build stops before the state commit, so the retained generation cannot observe state that belongs to the rejected preset.

### WinUI library restart

```text
folder-picker completion
  -> MainWindow requests an App-owned dispatcher handoff
  -> picker coroutine returns
  -> retire and release the current MainWindow and shell generation
  -> release LibrarySession, AppRuntime, and application-state stores
  -> launch the exact executable with the selected root
  -> exit the parent process
  -> successor builds and activates its only window/session graph
  -> persist the selected root best-effort and scan when needed
```

A successor graph never overlaps the parent graph or its `ConfigStore` writers on the supported restart path.
Target open failure belongs to successor startup and does not reconstruct the parent process.
Within either process, one build borrows explicit session- and coordinator-owned collaborators; their references remain valid until the owning window retires its shell and coordinator.

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
- For WinUI, one main window owns one live generation, built from the preset the resolved shell mode names. Modern is the default. Switching shell mode replaces that generation but cannot replace the window's `LibrarySession`, initiate scanning, or stop playback.
- WinUI responsive layout may collapse side regions but cannot hide the track list or playback controls. Classic Soul remains a separate 32 by 32 glyph-free control before ordinary Play/Pause and Stop controls.
- `LibraryWindowSession` owns one immutable window/session relationship; `App` releases its window and then session before launching a successor process.
- One `UiCoordinator` owns the session callback sink and borrowed runtime consumers for one WinUI main window.
- WinUI state sources outlive the layout host; subscriptions are generation-owned members and are destroyed before those sources.
- Shell state is committed only after any required preset candidate publishes successfully, and equivalent resolved state is not emitted again.
- A WinUI `LayoutBuildContext` is bounded to one generation build and is not retained; each constructed component stores only the narrow references and subscriptions it needs.
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

WinUI unregisters XAML, runtime, playback, SMTC, and cover-art observations before its window releases the borrowed session. The main window retires `ShellBuilder` first; destroying the live generation releases its scoped shell-state and track-list subscriptions while their `ShellBuilder`- and `UiCoordinator`-owned signal sources still exist. `UiCoordinator::retire()` then clears the session callback registration and unbinds its owned collaborators. Playback leaf destructors unbind their ViewModels before revoking native event tokens, while the main window's XAML tree is still alive.

WinUI library restart is requested through the dispatcher so the folder-picker callback returns before teardown.
The parent retires every shell and runtime borrower, releases its window/session graph and application-state writers, and only then starts the successor.
Target creation or configuration failure is presented by that successor without parent rollback.
The selected root is persisted only after successor activation; its initial scan then runs against the active successor and remains retryable after failure.

## Implementation map

- [`LayoutDocument`](../../app/include/ao/uimodel/layout/document/LayoutDocument.h), [`LayoutNode`](../../app/include/ao/uimodel/layout/document/LayoutNode.h), and [`LayoutPreparation`](../../app/include/ao/uimodel/layout/document/LayoutPreparation.h) own the platform-neutral document and preparation proof.
- UIModel layout action, component, document, and shell types live under [`app/include/ao/uimodel/layout/`](../../app/include/ao/uimodel/layout/) and [`app/uimodel/layout/`](../../app/uimodel/layout/).
- [`ShellLayoutController`](../../app/linux-gtk/app/ShellLayoutController.h) is the current GTK shell owner.
- [`ComponentRegistry`](../../app/linux-gtk/layout/runtime/ComponentRegistry.h), [`ActionRegistry`](../../app/linux-gtk/layout/runtime/ActionRegistry.h), [`LayoutRuntime`](../../app/linux-gtk/layout/runtime/LayoutRuntime.h), and [`LayoutHost`](../../app/linux-gtk/layout/runtime/LayoutHost.h) own GTK construction and activation.
- [`ShellLayoutStore`](../../app/linux-gtk/app/ShellLayoutStore.h) and [`ShellLayoutComponentStateStore`](../../app/linux-gtk/app/ShellLayoutComponentStateStore.h) own customized layouts and component state.
- [`KeymapModel`](../../app/include/ao/uimodel/input/KeymapModel.h), [`KeymapApplicator.cpp`](../../app/linux-gtk/app/KeymapApplicator.cpp), and [`ShortcutEditorWidget.cpp`](../../app/linux-gtk/preference/ShortcutEditorWidget.cpp) own neutral policy and GTK adaptation.
- [`app/windows-winui/CMakeLists.txt`](../../app/windows-winui/CMakeLists.txt) places all compiled WinUI implementation in the Windows-only `aobus-winui-lib` static library and leaves `aobus-winui` as the final-link and deployed-resource boundary.
- [`App`](../../app/windows-winui/App.xaml.h), [`LibraryWindowSession`](../../app/windows-winui/app/LibraryWindowSession.h), [`LibrarySession`](../../app/windows-winui/app/LibrarySession.h), [`ProcessLauncher`](../../app/windows-winui/platform/ProcessLauncher.h), and [`MainWindow`](../../app/windows-winui/MainWindow.xaml) own the WinUI shell and destructive library restart; shell, track, and playback code-behind methods are compiled from their matching subsystem directories.
- [`UiCoordinator`](../../app/windows-winui/app/UiCoordinator.h) owns the WinUI session callback boundary and exposes its track-list, resource-byte, and theme collaborators through narrow accessors.
- [`LayoutBuildContext`](../../app/windows-winui/layout/runtime/LayoutBuildContext.h) is the per-generation construction carrier, while [`ShellLibraryAccess`](../../app/windows-winui/layout/runtime/ShellLibraryAccess.h) limits session-backed component operations to library-root display, list projection, presentation preference, and track playback.
- Playback leaves bind directly to `PlaybackService` and, when required, `PlaybackCommandSurface`; [`AssertWinUiLeafCapabilities.cmake`](../../cmake/AssertWinUiLeafCapabilities.cmake) prevents generation components and leaf adapters from regaining session/runtime composition authority.
- [`ShellBuilder`](../../app/windows-winui/layout/ShellBuilder.h) owns WinUI shell composition, [`ShellPresetSource`](../../app/windows-winui/layout/ShellPresetSource.h) reads the packaged presets, and [`LayoutHost`](../../app/windows-winui/layout/runtime/LayoutHost.h) keeps exactly one generation attached to the frame.
- [`TransportButton`](../../app/windows-winui/playback/TransportButton.h), [`SoulTransportButton`](../../app/windows-winui/playback/SoulTransportButton.h), [`OutputDeviceControl`](../../app/windows-winui/playback/OutputDeviceControl.h), [`SeekControl`](../../app/windows-winui/playback/SeekControl.h), [`PlaybackTimeControl`](../../app/windows-winui/playback/PlaybackTimeControl.h), and [`VolumeControl`](../../app/windows-winui/playback/VolumeControl.h) own playback leaf adaptation; a document's playback components compose them.
- [`ShellStatePolicy`](../../app/windows-winui/include/ao/winui/layout/ShellStatePolicy.h) owns the Windows-only shell-state decision, while [`AobusSoulViewModel`](../../app/include/ao/uimodel/playback/soul/AobusSoulViewModel.h) owns shared Soul policy.

## Test map

- UIModel layout tests under [`test/unit/uimodel/layout/`](../../test/unit/uimodel/layout/) protect document, bounded preparation, templates, catalogs, actions, component-state, promotion, and session policy.
- GTK layout runtime and component tests under [`test/unit/linux-gtk/layout/`](../../test/unit/linux-gtk/layout/) protect construction, registry injection, actions, surfaces, editor behavior, and component state.
- [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) protects shell ownership by the window.
- Keymap tests under [`test/unit/uimodel/input/`](../../test/unit/uimodel/input/) and [`ShortcutEditorWidgetTest.cpp`](../../test/unit/linux-gtk/preference/ShortcutEditorWidgetTest.cpp) protect neutral and GTK shortcut boundaries.
- The UIModel organization guardrail in [`AssertUimodelOrganization.cmake`](../../cmake/AssertUimodelOrganization.cmake) protects platform-neutral placement.
- [`AssertWinUiStateSubscriptions.cmake`](../../cmake/AssertWinUiStateSubscriptions.cmake) prevents the removed WinUI virtual-callback and raw-observer fan-out contract from returning on platforms without a native widget-test host.
- [`AssertWinUiLeafCapabilities.cmake`](../../cmake/AssertWinUiLeafCapabilities.cmake) enforces narrow WinUI generation and leaf dependencies during native builds.
- Windows-native tests under [`test/unit/winui/`](../../test/unit/winui/) protect breakpoints, persistence, theme fallback, startup/restart policy, shell vocabulary, and command-line behavior; they are included in `ao_core_test` only on Windows. Shared UIModel tests protect Soul constants, playback ViewModels, and bounded caches. Native `winui` Debug and Release builds protect `aobus-winui-lib`, XAML, generated C++/WinRT, PRI resources, and final executable composition; the current repository has no WinUI widget-test host.

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
