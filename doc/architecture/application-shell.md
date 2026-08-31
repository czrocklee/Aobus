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
It covers the GTK declarative layout session, schema, registries and factories, action activation and Gio export, per-build dependency wiring, component runtime state, editor rebuilds, shortcuts, and teardown. It also owns the WinUI Modern/Classic shell boundary, window/session ownership, responsive policy, and native adapter placement, plus the TUI's projection of neutral application shortcuts into terminal input.

It does not own the semantic state rendered by a track, playback, workspace, status, or resource component.
It does not make the GTK declarative layout system a cross-frontend contract: the document, schema values, and build-time state vocabulary are platform-neutral, but component construction is not, so WinUI builds its own presets against its own schema while TUI builds its terminal shell independently.

The subject qualifies as an application-system architecture because it has an independent document, schema, construction, action, persistence, rebuild, and lifetime graph that presentation alone cannot explain without absorbing GTK shell orchestration.

## System context

The [architecture landscape](README.md) classifies the application shell as an application system.
The [system architecture](system-overview.md) places reusable layout values and policies in UIModel and concrete widget construction in the GTK frontend.

```text
UIModel
  LayoutDocument -> bounded preparation -> PreparedLayout
  LayoutSchema: component + action vocabulary, including shared entries
  LayoutSession -> LayoutBuildSnapshot + ComponentStateBinding
  component-state policy + keymap model
           |
           v
GTK MainWindow owns ShellLayoutController
  ComponentRegistry: schema entry -> GTK factory
  ActionRegistry: action id -> handler/state provider
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
  -> aobus-winui-lib owns shell-state rules + settings/theme schemas
  -> MainWindow owns the XAML frame, its resources, and one layout host
     -> session callbacks, TrackListController, theme coordinator
        -> cover-art presenters borrow the runtime-owned loader and window-owned theme
     -> ShellBuilder owns schema, actions, menus, pane accessors, and observable shell state
        -> component factories capture exact window/session services at registration
        -> LayoutBuildContext carries only generation-scoped build values
        -> one live generation built from the preset the resolved mode names
        -> playback leaf controls own native event tokens + UIModel bindings
     -> mode switch replaces the whole generation
  -> SMTC, picker, AppWindow, and WASAPI adapters remain Windows-local
```

## Responsibilities

### Platform-neutral shell language

UIModel owns `LayoutDocument`, `LayoutNode`, `LayoutValue`, shell-owned preparation limits, bounded template expansion, `PreparedLayout`, node-id validation, `LayoutSchema`, component-state documents and promotion policy, `LayoutSession`, and keymap values and policy.

`LayoutSchema` is the inert component-and-action vocabulary an authored document may use; executable callbacks remain frontend-local. `LayoutSession` owns the active document, component state, edit state, and generation. Each build receives one immutable owning `LayoutBuildSnapshot`, and each stateful component retains a `ComponentStateBinding` whose generation fence controls restoration and writes.

These values describe structure, stable command identity, validation metadata, and UI-local state without naming GTK widget classes, GDK key symbols, or runtime storage objects beyond narrow managed-state adapters.
Stable action, component, command, and shortcut identities are never localized.
Frontend composition supplies localized shell labels at the leaf: GTK and TUI use their process-lifetime typed text catalogs, while WinUI shell construction resolves generated MRT resources from its explicit locale context.
The GTK Layout Editor likewise retains component types, property names, enum values, action ids, and node ids as document identity while its adapter resolves built-in display names, field labels, and enum choices from the injected catalog; unknown extension vocabulary remains visible unchanged.

### Windows shell composition contracts

[Decision 0004](../decision/0004-adopt-layout-documents-for-winui-shell-composition.md) makes the version 1 layout language the Windows shell composition language while leaving construction with the frontend.
That boundary has two halves, and the split is by who decides rather than by what compiles.
UIModel owns what both shells decide the same way: reading the version 1 common layout fields once, walking a candidate against a schema and a dialect, the parse-expand-validate step, the `ShellGenerationSequence` that keeps exactly one view generation live, and the canonical component entries whose meaning both shells share.
The Windows-only `aobus-winui-lib` owns what is this shell's own: which shared entries it imports and extends, the types and actions only Windows has, the component-to-element mapping, the layout dialect that extends the shared rules with `styleKey` and themed surfaces, `styleKey` lookup planning, the WinUI interpretation of the common fields, and the shell-state rules for native-window breakpoints and pane modes.
The two built-in preset documents ship under `app/windows-winui/layout/`.
The [Windows layout schema reference](../reference/windows/layout-schema.md) owns those exact surfaces.

Both halves name XAML type and resource-scope identities as values rather than as C++/WinRT types, so the pure rules remain testable without constructing a XAML host.
Being portable at the source level is not what makes something shared: a schema builder that registers `windows.navigationPane` speaks for one shell however cleanly it compiles, so it lives in `aobus-winui-lib`.
Where it is gated is a separate question from who owns it. The Windows schema carries no WinRT, and what it has to get right - that a type both shells present keeps one meaning - is exactly what a change made on Linux can break unseen, so it is built and tested on every host.
Sharing the language is not sharing a runtime: there is no cross-frontend build plan or responsive classifier, GTK uses none of these contracts, and Windows keeps native construction, parent placement, controller binding, generation-owned view adapters and the focused-selection projection its components share, `winui::classifyShellWidth()` boundaries, and `winui::DesktopSettings` pane persistence.

The WinUI window builds its shell from the selected preset. `MainWindow.xaml` keeps the window frame, the single layout host, `RootGrid.Resources`, styles, and compiled `DataTemplate` resources; it composes no shell of its own.

### GTK shell owner

`MainWindow` owns one `ShellLayoutController` for the complete window lifetime.
The controller is the current GTK shell composition owner: it selects and loads a preset, owns the `LayoutSession`, component and action registries, layout host, editor workflow, stores, action export, and the borrowed collaborators it hands to component registration and reads from its own action handlers.

This is a broad current responsibility set, not a general-purpose runtime facade.
Customized-layout load/save/remove operations and candidate preparation return typed results; component-state persistence retains its narrower optional/Boolean contract.
The underlying grouped store provides fail-closed one-shot replacement without generic commit receipts or a blocked-store recovery mode.

### WinUI shell owner

`App` owns the dispatcher and one `LibraryWindowSession` in destruction-safe order.
The window session owns exactly one `LibrarySession` and `MainWindow`; the session owns exactly one runtime for the process lifetime, and neither owner can retarget or replace it.
Opening another root queues an application-level destructive restart rather than constructing another graph in the process.
Shell mode changes never replace the session.
`MainWindow` owns the XAML frame, its resource scope, the single layout host, the commands a preset can name, and the window's own runtime consumers; it holds no shell composition of its own.

`ShellBuilder` is the window's shell-lifetime composition owner. It holds the schema, action registry, menu composition, `winui::DesktopSettings` pane accessors, current shell state, retained status message, native-window activity, their change signals, and the layout host. `LayoutHost` only stages and publishes generations; it does not relay state into their components. The builder decides whether the target shell mode requires a new preset generation and commits a changed shell state only after any required candidate publishes. A build that fails leaves the current generation and its committed state live and reports why; a first build that fails leaves the frame showing a minimal layout-error surface, because a shipped document that does not build is an artifact defect rather than a user-recoverable state.

`MainWindow` is also the one window-scoped owner of the `LibrarySession` status/failure callbacks, the reveal-request subscription, and the track-list, resource-byte, and theme consumers those callbacks and the shell read.
They are constructed from the borrowed session when the window is initialized and released by `MainWindow::shutdown()`, which stops status publication and reveal routing before releasing anything they publish into.
There is no separate coordinator layer between the window and its consumers: they have exactly the window's lifetime, so `MainWindow` is their owner.
`ShellBuilder` resolves its existing `LibrarySession` into individual callbacks when it registers component families. Each factory captures only the callbacks and services its component needs: the library-path factory retains only the root path, navigation retains list projection, invalidation, presentation, and authoring operations, and track components retain only their own playback, membership, ordering, or creation operations. The per-build `LayoutBuildContext` carries generation-scoped values, and generation components never receive `LibrarySession` or `AppRuntime`.
Leaf controls receive the exact runtime or UIModel service their binding needs.

Playback buttons, Soul transport, seek, time, and volume adapters are leaf controls. Each leaf owns every XAML event token it registers and its corresponding UIModel ViewModel, starts every bind with an idempotent unbind, and reconciles the native control from the ViewModel's initial callback. Modern and Classic use separate native adapters even when they render the same semantic command.

A WinUI component observes mutable UI state through a current-value-plus-signal contract. It reads the current value during construction, connects directly to the owning source, and retains the returned `async::Subscription` as a component member. `ShellBuilder` directly owns shell state, native-window activity, transient status, and their signals; `TrackListController` owns track-list changes. Those owners outlive `LayoutHost`, so generation destruction releases every subscription before its source. There is no generic shell-state coordinator, component observer interface, raw observer list, or host fan-out path to synchronize with tree replacement.

`aobus-winui-lib` owns this shell's responsive breakpoints and strict Windows settings and theme schemas, because only this shell uses or persists them.
Shared UIModel values own Soul constants and animation gating, and bounded row and artwork caching.
WinUI also owns XAML controls, HWND and `AppWindow` adaptation, `DispatcherQueue` delivery, FolderPicker, SMTC, and WASAPI provider registration.

### Component and action registries

The GTK `ComponentRegistry` pairs each platform-neutral `ComponentSchema` with one GTK `ComponentFactory`.
Its embedded `LayoutSchema` is the editor and validator authority for registered type metadata.
`ComponentSchema::actionId()` returns a borrowed view into either the selected `LayoutNode` property or schema default; callers copy it before mutating either owner, crossing reentrant work, suspending, or retaining it beyond that synchronous resolution.

The GTK `ActionRegistry` adds each `ActionSchema` to that same schema and retains one handler and optional availability provider.
It remains the live command authority; layout nodes, keyboard maps, and Gio actions refer to stable action ids instead of duplicating command behavior.
Every action exported to a longer-lived Gio action map is owned by a scoped registration that retains the exact action and activation connection.
Registration retirement disconnects handlers before removing only the exact actions it installed, so an independently retained old action is inert and an overlapping replacement with the same id remains installed.
`ActionActivationContext` is a synchronous borrowed view for one activation or availability query; handlers cannot retain the context or its window and anchor references.

### Layout construction

`prepareLayout()` meters the authored document, performs bounded template expansion, and produces the only value accepted by `LayoutRuntime` and `LayoutHost`.
`LayoutRuntime` recursively requests GTK components from that prepared effective root.
`LayoutHost` owns the active component tree and exposes a staged `prepare`/`commit` boundary for load, editor preview/save, reset, and panel-size promotion.

Factories receive one `LayoutBuildContext` assembled for the build.
It carries the build environment, borrowed `LayoutSession`, owning immutable `LayoutBuildSnapshot`, candidate-scoped `SharedWidgetHandoff`, and traversal-local detail and scheduling state. Required session-lifetime collaborators are captured into factory lambdas at registration time rather than reached through a global service locator or dependency bag.

Registration-time capture makes collaborator availability a construction-order obligation of the window.
Every shell-owned collaborator a component names, the application menu model included, is supplied in the `ShellLayoutCollaborators` argument the controller is constructed with; a collaborator the window only produces afterwards never reaches the already-registered factories.

### State and input adaptation

Component runtime state is separate from authored layout structure.
Stateful components resolve state by stable node id, component type, state version, and a baseline hash, then persist interaction state through the shell-lifetime store.
`ComponentStateBinding` owns that resolution and the write guards, so a frontend component declares only *what* it persists.
It refuses to write on a tooltip surface, in edit mode, for an anonymous node, without an active preset or store, and once the shell has replaced the state document — the captured generation no longer matches, so a component destructing after a reset, load, or save-defaults cannot pollute the new document.

The keymap model binds neutral chords to action ids.
GTK translates those chords to native accelerator syntax and applies eligible window actions; the shortcut editor remains a GTK view over UIModel policy.
WinUI translates the same neutral values into the keyboard accelerators its live action registry can execute.
TUI begins with the shared application defaults, adds terminal-only defaults in its adapter, loads the global override group, and builds one immutable `TuiKeymapPlan`.
That plan considers only actions with a TUI handler, resolves collisions after FTXUI projection, and supplies both root dispatch and every configurable shortcut hint.
Terminal input protocol remains a narrower scope above the plan: Ctrl-C, text editing and completion, list and modal-overlay navigation/activation, notification `x`, mouse sequences, and escape routes cannot be disabled by a root binding.

## Boundaries and dependency direction

- UIModel shell types may depend on stable runtime values and managed-state mechanisms but never on GTK, GDK, Gio, or frontend-local classes.
- Build-time state, surface, and action-slot vocabulary lives in UIModel and is usable without a widget toolkit. Whether a second frontend adopts it is a separate decision; hoisting the vocabulary does not by itself make the component system cross-frontend.
- GTK layout registries depend on UIModel schema values and concrete GTK factories; `LayoutSchema` does not depend back on those registries.
- GTK component factories capture the exact runtime/UIModel collaborators they name when they are registered, never from `LayoutBuildContext`; runtime services never depend on layout components.
- Layout documents carry stable component types, properties, action ids, and semantic CSS class values, not C++ factory names or widget pointers.
- The action registry owns activation and availability; a component binding or keymap is a reference, not a parallel handler.
- Gio export and native accelerator application stop at their frontend boundaries.
- TUI reuses neutral action and keymap values deliberately, but FTXUI events, terminal aliases, TUI-only action ids, and the immutable terminal plan remain frontend-private; the GTK document cannot be described as TUI shell authority.
- WinUI does not parse or adapt the GTK layout document. It shares the layout language and genuinely cross-frontend semantic policy, while keeping its presets, shell-state policy, schema extensions, and factories, without introducing a second runtime authority.
- WinUI component registration captures explicit borrowed services and individual callback values; its `LayoutBuildContext` carries only generation-scoped build values. Generation components and leaf adapters cannot accept `LibrarySession`, `AppRuntime`, or a complete window dependency graph.
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
  -> controller applies document/state/generation to LayoutSession
  -> LayoutHost commits the prepared tree and its widget handoff
```

### Action route

```text
layout component, menu, shortcut, or Gio action id
  -> ActionRegistry validation and availability
  -> activation context with window, anchor, and component id
  -> registered handler
  -> runtime/UIModel command or GTK-local surface
```

Gio export includes only actions for which the shell can provide the required anchor/menu context.
The bridge refreshes enabled state from the live registry while its scoped export remains the exact action currently installed under that id.
Replacing or ending an export first disconnects its activation handlers and then unexports its exact actions.

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
  -> winui::resolveShellState() resolves the target state
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
- Every GTK action-map registration retires before both its map and callback target; a self-attached action group closes in its derived owner before member and GTK-base teardown.
- `LayoutSession` is the active preset/document/state/generation authority; editor working copies, build snapshots, and preview trees are not authoritative.
- Raw `LayoutDocument` values remain authored/session/persistence values; only `PreparedLayout` may enter GTK construction.
- `LayoutBuildContext` is created for one recursive build and cannot be retained as shell wiring.
- Candidate construction reads preset, component state, edit mode, callbacks, and generation from its owning immutable `LayoutBuildSnapshot`.
- `LayoutSession` outlives every `ComponentStateBinding` that can write component state.
- Stateful component identity is a stable expanded node id, never tree position.
- Anonymous stateful nodes remain usable but non-persistent; duplicate stateful ids are invalid for save.
- Component-state generation prevents an old tree from writing into a newly installed state document.
- A detached tree captures the next generation; commit advances that generation before destroying the retiring tree.
- A GTK candidate that reparents a shell-owned singleton widget carries one `SharedWidgetHandoff`; rejection rolls the transfer back, while commit finalizes it before the retiring tree is destroyed.
- Authored layouts, component runtime state, global preset selection, and keyboard overrides remain separate persistence classes.
- Unknown component and template references become visible diagnostic components rather than undefined factory calls.
- For WinUI, one main window owns one live generation, built from the preset the resolved shell mode names. Modern is the default. Switching shell mode replaces that generation but cannot replace the window's `LibrarySession`, initiate scanning, or stop playback.
- WinUI responsive layout may collapse side regions but cannot hide the track list or playback controls. Classic Soul remains a separate 32 by 32 glyph-free control before ordinary Play/Pause and Stop controls.
- `LibraryWindowSession` owns one immutable window/session relationship; `App` releases its window and then session before launching a successor process.
- One WinUI `MainWindow` owns the session callback sink and the runtime consumers for its own window, and names each retirement step rather than relying on member destruction order.
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

During teardown the controller closes callback admission, releases action-state subscriptions, and retires its Gio export before clearing the host while the layout session, stores, registries, and borrowed dependencies are still alive.
The final clear does not advance the state generation, allowing the current component tree to flush pending state before its owners disappear.
`MainWindow` retires its List-navigation and window-action registrations before their controllers, registry, and inherited action map, while the Layout Editor disconnects and detaches its self-owned action group in the derived destructor.
Editor theme and callback tokens are released before the controller's collaborators.

WinUI unregisters XAML, runtime, playback, SMTC, and cover-art observations before its window releases the borrowed session. The main window retires `ShellBuilder` first; destroying the live generation releases its scoped shell-state and track-list subscriptions while their `ShellBuilder`- and window-owned signal sources still exist. `MainWindow::shutdown()` then unbinds the XAML-owned playback controls, clears the session callback registration, releases the reveal subscription, and only then releases the resource, theme, and track-list consumers. Constructor-bound leaf destructors stop their ViewModels before revoking native event tokens, while the main window's XAML tree is still alive.

WinUI library restart is requested through the dispatcher so the folder-picker callback returns before teardown.
The parent retires every shell and runtime borrower, releases its window/session graph and application-state writers, and only then starts the successor.
Target creation or configuration failure is presented by that successor without parent rollback.
The selected root is persisted only after successor activation; its initial scan then runs against the active successor and remains retryable after failure.

## Implementation map

- [`LayoutDocument`](../../app/include/ao/uimodel/layout/document/LayoutDocument.h), [`LayoutNode`](../../app/include/ao/uimodel/layout/document/LayoutNode.h), and [`LayoutPreparation`](../../app/include/ao/uimodel/layout/document/LayoutPreparation.h) own the platform-neutral document and preparation proof.
- [`LayoutSchema`](../../app/include/ao/uimodel/layout/component/LayoutSchema.h) owns component and action vocabulary and validation; [`LayoutSession`](../../app/include/ao/uimodel/layout/shell/LayoutSession.h) owns the active layout lifetime, build snapshots, and state bindings.
- UIModel layout action, component, document, and shell types live under [`app/include/ao/uimodel/layout/`](../../app/include/ao/uimodel/layout/) and [`app/uimodel/layout/`](../../app/uimodel/layout/).
- [`ShellLayoutController`](../../app/linux-gtk/app/ShellLayoutController.h) is the current GTK shell owner; [`GioActionBridge`](../../app/linux-gtk/layout/runtime/GioActionBridge.h) and [`ActionMapRegistration`](../../app/linux-gtk/common/ActionMapRegistration.h) own bounded Gio export and exact-identity registration teardown.
- [`ComponentRegistry`](../../app/linux-gtk/layout/runtime/ComponentRegistry.h), [`ActionRegistry`](../../app/linux-gtk/layout/runtime/ActionRegistry.h), [`LayoutRuntime`](../../app/linux-gtk/layout/runtime/LayoutRuntime.h), and [`LayoutHost`](../../app/linux-gtk/layout/runtime/LayoutHost.h) own GTK construction and activation.
- [`ShellLayoutStore`](../../app/linux-gtk/app/ShellLayoutStore.h) and [`ShellLayoutComponentStateStore`](../../app/linux-gtk/app/ShellLayoutComponentStateStore.h) own customized layouts and component state.
- [`KeymapModel`](../../app/include/ao/uimodel/input/KeymapModel.h), [`KeymapApplicator.cpp`](../../app/linux-gtk/app/KeymapApplicator.cpp), and [`ShortcutEditorWidget.cpp`](../../app/linux-gtk/preference/ShortcutEditorWidget.cpp) own neutral policy and GTK adaptation.
- [`app/windows-winui/CMakeLists.txt`](../../app/windows-winui/CMakeLists.txt) places all compiled WinUI implementation in the Windows-only `aobus-winui-lib` static library and leaves `aobus-winui` as the final-link and deployed-resource boundary.
- [`App`](../../app/windows-winui/App.xaml.h), [`LibraryWindowSession`](../../app/windows-winui/app/LibraryWindowSession.h), [`LibrarySession`](../../app/windows-winui/app/LibrarySession.h), [`ProcessLauncher`](../../app/windows-winui/platform/ProcessLauncher.h), and [`MainWindow`](../../app/windows-winui/MainWindow.xaml) own the WinUI shell and destructive library restart; shell, track, and playback code-behind methods are compiled from their matching subsystem directories.
- [`MainWindow.xaml.cpp`](../../app/windows-winui/MainWindow.xaml.cpp) owns the WinUI session callback boundary along with the track-list, resource-byte, and theme consumers, and states its retirement order in `shutdown()`.
- [`LayoutBuildContext`](../../app/windows-winui/layout/runtime/LayoutBuildContext.h) is the per-generation construction carrier; [`ShellBuilder`](../../app/windows-winui/layout/ShellBuilder.h) binds session-backed operations into exact callback captures when it registers each component family.
- Playback leaves receive `PlaybackService` and, when required, `PlaybackActions` in their constructors; only XAML runtime classes retain explicit bind/unbind seams. [`AssertWinUiLeafCapabilities.cmake`](../../cmake/AssertWinUiLeafCapabilities.cmake) prevents generation components and leaf adapters from regaining session/runtime composition authority.
- [`ShellBuilder`](../../app/windows-winui/layout/ShellBuilder.h) owns WinUI shell composition, [`ShellPresetSource`](../../app/windows-winui/layout/ShellPresetSource.h) reads the packaged presets, and [`LayoutHost`](../../app/windows-winui/layout/runtime/LayoutHost.h) keeps exactly one generation attached to the frame.
- [`TransportButton`](../../app/windows-winui/playback/TransportButton.h), [`SoulTransportButton`](../../app/windows-winui/playback/SoulTransportButton.h), [`OutputDeviceControl`](../../app/windows-winui/playback/OutputDeviceControl.h), [`SeekControl`](../../app/windows-winui/playback/SeekControl.h), [`PlaybackTimeControl`](../../app/windows-winui/playback/PlaybackTimeControl.h), and [`VolumeControl`](../../app/windows-winui/playback/VolumeControl.h) own playback leaf adaptation; a document's playback components compose them.
- [`ShellState`](../../app/windows-winui/include/ao/winui/layout/ShellState.h) owns the Windows-only shell-state values and decisions, while [`AobusSoulViewModel`](../../app/include/ao/uimodel/playback/soul/AobusSoulViewModel.h) owns shared Soul behavior.

## Test map

- UIModel layout tests under [`test/unit/uimodel/layout/`](../../test/unit/uimodel/layout/) protect document, bounded preparation, templates, schema, actions, component state, promotion, and session policy.
- GTK layout runtime and component tests under [`test/unit/linux-gtk/layout/`](../../test/unit/linux-gtk/layout/) protect construction, registry injection, actions, surfaces, editor behavior, and component state.
- [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp), [`MenuControllerTest.cpp`](../../test/unit/linux-gtk/app/MenuControllerTest.cpp), [`ShellLayoutControllerTest.cpp`](../../test/unit/linux-gtk/app/ShellLayoutControllerTest.cpp), and [`GioActionBridgeTest.cpp`](../../test/unit/linux-gtk/layout/runtime/GioActionBridgeTest.cpp) protect scoped action export, replacement identity, retained-action revocation, and window/controller teardown.
- Keymap tests under [`test/unit/uimodel/input/`](../../test/unit/uimodel/input/), [`ShortcutEditorWidgetTest.cpp`](../../test/unit/linux-gtk/preference/ShortcutEditorWidgetTest.cpp), and [`TuiKeymapTest.cpp`](../../test/unit/tui/TuiKeymapTest.cpp) protect neutral, GTK, and terminal shortcut boundaries.
- The UIModel organization guardrail in [`AssertUimodelOrganization.cmake`](../../cmake/AssertUimodelOrganization.cmake) protects platform-neutral placement.
- [`AssertWinUiStateSubscriptions.cmake`](../../cmake/AssertWinUiStateSubscriptions.cmake) prevents the removed WinUI virtual-callback and raw-observer fan-out contract from returning on platforms without a native widget-test host.
- [`AssertWinUiLeafCapabilities.cmake`](../../cmake/AssertWinUiLeafCapabilities.cmake) enforces narrow WinUI generation and leaf dependencies during native builds.
- [`AssertGtkLeafCapabilities.cmake`](../../cmake/AssertGtkLeafCapabilities.cmake) keeps `AppRuntime` and the aggregate GTK dependency bag at registration composition roots, so component leaves receive exact services.
- Tests under [`test/unit/winui/`](../../test/unit/winui/) protect breakpoints, persistence, theme fallback, startup/restart policy, shell vocabulary, and command-line behavior. Those needing a native host are included in `ao_core_test` only on Windows; Windows shell policy carrying no WinRT dependency - settings compatibility, output-preference resolution, root-commit sequencing, the component schema, and the keyboard-accelerator plan - is compiled and run on every host, because those are the rules a Linux-only change is most likely to break unnoticed. Shared UIModel tests protect Soul constants, playback ViewModels, and bounded caches. Native `winui` Debug and Release builds protect `aobus-winui-lib`, XAML, generated C++/WinRT, PRI resources, and final executable composition; the current repository has no WinUI widget-test host.

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
- [GTK layout schema and action reference](../reference/shell/layout-schema.md)
- [Shared component vocabulary](../reference/shell/component-vocabulary.md)
- [Keyboard map reference](../reference/shell/keymap.md)
- [Windows desktop shell specification](../spec/shell/windows-desktop.md)
- [Windows desktop state reference](../reference/windows/desktop-state.md)
