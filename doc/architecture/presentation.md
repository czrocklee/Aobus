---
id: architecture.presentation
type: architecture
status: current
domain: presentation
summary: Defines responsibility boundaries among runtime state, platform-neutral UIModel policy, and GTK, TUI, and CLI adapters.
---
# Presentation architecture

## Scope

This document owns the boundary between frontend-neutral application state, platform-neutral presentation policy, and platform-specific adapters.
It defines what belongs in runtime, UIModel, GTK, TUI, and CLI and how state and commands cross those boundaries.

It does not define exact layouts, widget behavior, terminal key bindings, command syntax, display strings, or editor validation rules.
Those facts belong in specifications, reference documents, and user/development guides.
It also does not own track-source membership or expression semantics; those boundaries belong to the [library](library.md) and [track expression](track-expression.md) architectures.
Workspace authority belongs to the [workspace architecture](workspace.md); interactive runtime composition, startup restoration, and active-library replacement belong to the [interactive session lifecycle architecture](interactive-session-lifecycle.md).
The declarative layout document, shell session, action/component registries, GTK factory graph, component state, and shortcut adaptation belong to the [application shell architecture](application-shell.md).

## System context

Interactive presentation is a one-way dependency stack:

```text
runtime state and commands
          |
          v
platform-neutral UIModel
       /       \
      v         v
    GTK         TUI

CLI -> runtime directly for non-interactive tasks
```

Runtime remains authoritative for state that changes application behavior across frontends.
UIModel turns that state into reusable view state and user-interaction policy.
GTK and TUI adapt those values to toolkit lifecycle, rendering, input, timing, and native resources.

## Responsibilities

### Runtime presentation inputs

Runtime owns canonical identities, workspace/view lifecycle under the [workspace architecture](workspace.md), structural presentation specifications, live source/projection state, playback state, notifications, and commands that mutate application behavior.
It exposes typed snapshots and subscriptions without naming widgets, CSS classes, terminal cells, or native icons.

For track-list views, runtime keeps content and shape as separate state axes.
`listId` and `filterExpression` select a source, while `TrackPresentationSpec` selects sorting, grouping, visible fields, and redundant-field suppression.
`LiveTrackListProjection` is their composition point, not a second authority for either concern.

### UIModel

UIModel owns deterministic platform-neutral presentation behavior.
Its feature capsules contain view models, editor/form models, interaction models, policies, projections, formatters, catalogs, resolvers, and UI-local stores.

UIModel may subscribe to runtime services, combine several runtime snapshots, format display values, maintain an edit draft or gesture, and emit a runtime command or typed edit result.
It does not own storage transactions, playback succession, audio control, runtime retry policy, or platform lifecycle.

UIModel may resolve quick-search text into a core query expression and may inspect a valid Smart List expression to recommend a presentation.
Those are authoring and recommendation policies: UIModel does not evaluate membership or redefine query grammar.

Metadata editors currently derive patches from a detail snapshot but submit only target ids and patch values; the runtime command does not carry the snapshot baseline or library generation.
[RFC 0023](../rfc/0023-revision-bound-metadata-authoring.md) proposes the guarded authoring contract without moving storage transactions into UIModel.

The public namespace remains `ao::uimodel`; feature ownership is expressed by singular folders mirrored across public headers, sources, and tests.

### GTK

GTK owns the desktop toolkit boundary: application/window lifetime, widgets, GObject/Gio models, CSS, dialogs, portals, MPRIS, native icons, main-context scheduling, and GTK-specific layout construction.
The interactive session lifecycle architecture owns how that platform lifetime composes and replaces the library-bound runtime; presentation owns how the live pair is adapted and rendered.

The application shell architecture refines the layout document, action/component metadata, GTK registries and builders, component state, and build context.
Presentation owns the semantic state that those shell components adapt and render.

`MainWindow` owns the visible window composition.
`MainWindowCoordinator` binds runtime/UIModel collaborators to that composition and remains an explicitly isolated migration seam for direct-library integration.
The smart-list preview dialog is another current seam because it constructs runtime evaluator/source objects against `MusicLibrary`; this is localized behavior, not the normal widget boundary.

### TUI

TUI owns FTXUI components, terminal geometry, key/mouse routing, overlays, refresh timing, and terminal-specific image rendering.
It constructs the same `AppRuntime`, uses shared runtime services and selected UIModel view models/policies, and builds terminal elements from their state.

TUI-local interaction models may own transient shell/overlay state but cannot become authorities for runtime playback, source order, or persisted library data.

### CLI

CLI is an application adapter rather than an interactive presentation layer.
It parses commands, invokes `CoreRuntime` library facilities, and encodes plain, YAML, or JSON output.
It bypasses UIModel because it does not maintain a reusable interactive view state.
Its structured automation DTOs are currently unversioned; [RFC 0029](../rfc/0029-versioned-cli-automation-protocol.md) proposes an explicit protocol envelope and compatibility policy.

## Boundaries and dependency direction

- Runtime has no dependency on UIModel or frontend code.
- UIModel depends on runtime interfaces and stable core value types, never platform UI libraries.
- GTK and TUI may depend on runtime and UIModel and own all platform resources.
- UIModel cannot include direct LMDB stores or audio player/engine/backend control headers.
- A frontend adapter translates one platform event into a UIModel/runtime action and translates semantic state into platform representation.
- Equivalent cross-frontend behavior uses the same runtime/UIModel authority instead of parallel frontend policy.
- Presentation affects ordering, grouping, visible fields, and rendering but never changes source membership.
- Expression formatting that produces a scalar CLI string is owned by the track expression system and is not a presentation spec or UI column model.
- CLI command and output inventories remain reference concerns even though their adapter code lives at the frontend edge.

## Data and control flow

Presentation state flows outward:

```text
runtime snapshot/event
  -> UIModel projection when shared presentation policy is needed
  -> GTK widget binding or TUI render function
```

User intent flows inward:

```text
GTK/TUI input event
  -> platform event translation
  -> UIModel interaction/editor policy when needed
  -> runtime command or typed mutation request
```

Purely platform concerns, such as CSS application, popover dismissal, terminal hit regions, and native file selection, stay within the frontend.
Purely structural layout concerns can travel through UIModel values, while GTK widget creation remains platform-owned.

For a track-list view, the two independent inputs meet in runtime before presentation state flows outward:

```text
ListId + filterExpression -> TrackSource membership
TrackPresentationSpec    -> projection shape
both                      -> rows and sections -> UIModel/frontend
```

A quick filter narrows the active membership while retaining the active presentation unless a separate presentation command changes it.

## Structural constraints

- UIModel values contain semantic presentation information, not platform handles or toolkit class names.
- A UIModel object can be unit-tested without a display server, terminal, storage environment, or audio backend unless it deliberately wraps a narrow runtime service.
- Frontends retain subscriptions and view models for no longer than the runtime services they observe.
- Runtime snapshots remain the source of truth after a frontend rebuilds its widget tree or terminal frame.
- UI-local persisted preferences influence presentation but do not replace canonical runtime state.
- Layout component factories receive an explicit dependency bundle and runtime-state carrier rather than reaching through global frontend singletons.
- Direct core-library access in current GTK migration seams is contained and cannot spread into ordinary widgets or UIModel.

## Failure, cancellation, and lifetime boundaries

Runtime failures arrive as typed results, snapshots, or observational events.
UIModel converts semantic state into platform-neutral display or action state but does not choose runtime recovery behavior.
Frontends decide how and where to render an error and own cancellation tied to widget/dialog/terminal lifetime.

GTK main-window teardown releases controllers, widgets, view models, and subscriptions before the window-owned `AppRuntime` is destroyed.
TUI releases its event/render collaborators before leaving the runtime scope.
Platform callbacks that can outlive a widget or controller use scoped subscriptions, cancellation handles, or weak ownership rather than raw lifetime assumptions.
`MainContextCallbackScope` provides the GTK-local weak lifetime boundary for void callbacks retained outside their owner.
`ImportExportCoordinator` uses that scope for its export-mode response and every native file-dialog completion, and supplies the native operations with a shared cancellation handle, while `ShortcutEditorWidget` uses it for delayed conflict responses.
Coordinator teardown closes the guard before requesting native cancellation, so every late callback is harmless even when cancellation loses the race.
The owner, teardown, and guarded callbacks are confined to one GLib main context; the scope does not provide cross-thread synchronization.

## Implementation map

- [`app/CMakeLists.txt`](../../app/CMakeLists.txt) defines and guards the runtime-to-UIModel dependency edge.
- [`app/include/ao/uimodel/`](../../app/include/ao/uimodel) and [`app/uimodel/`](../../app/uimodel) contain platform-neutral presentation capsules.
- [`MainWindow`](../../app/linux-gtk/app/MainWindow.h), [`MainWindowCoordinator`](../../app/linux-gtk/app/MainWindowCoordinator.h), and [`GtkUiDependencies`](../../app/linux-gtk/app/GtkUiDependencies.h) define GTK composition boundaries.
- [`MainContextCallbackScope`](../../app/linux-gtk/common/MainContextCallbackScope.h) bounds GTK-main-context callbacks to their owner lifetime.
- [`LayoutRuntime`](../../app/linux-gtk/layout/runtime/LayoutRuntime.h) and [`LayoutBuildContext`](../../app/linux-gtk/layout/runtime/LayoutBuildContext.h) build GTK layout values into widgets.
- [`app/tui/App.cpp`](../../app/tui/App.cpp) composes runtime, selected UIModel objects, terminal controllers, and rendering.
- [`CliRuntime`](../../app/cli/CliRuntime.h) is the non-interactive adapter boundary.
- [`AssertUimodelOrganization.cmake`](../../cmake/AssertUimodelOrganization.cmake) and [`AssertNoForbiddenIncludes.cmake`](../../cmake/AssertNoForbiddenIncludes.cmake) enforce organization and dependency constraints.

## Test map

- [`test/unit/uimodel/`](../../test/unit/uimodel) mirrors UIModel feature capsules and protects platform-neutral policy.
- [`MainWindowCoordinatorTest.cpp`](../../test/unit/linux-gtk/app/MainWindowCoordinatorTest.cpp) and [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) protect GTK composition.
- [`MainContextCallbackScopeTest.cpp`](../../test/unit/linux-gtk/common/MainContextCallbackScopeTest.cpp) protects callback invalidation and teardown ordering.
- [`ImportExportCoordinatorTest.cpp`](../../test/unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp) protects native chooser policy, handoff, and export-mode response invalidation.
- [`ShortcutEditorWidgetTest.cpp`](../../test/unit/linux-gtk/preferences/ShortcutEditorWidgetTest.cpp) protects delayed conflict-response invalidation.
- [`LayoutRuntimeBuildTest.cpp`](../../test/unit/linux-gtk/layout/components/LayoutRuntimeBuildTest.cpp) protects the UIModel-layout to GTK-widget boundary.
- [`LibraryControllerTest.cpp`](../../test/unit/tui/LibraryControllerTest.cpp) and [`TuiHitRegionsTest.cpp`](../../test/unit/tui/TuiHitRegionsTest.cpp) protect TUI runtime adaptation and terminal-only policy.
- [`CliSmokeTest.cpp`](../../test/unit/cli/CliSmokeTest.cpp) protects non-interactive runtime adaptation.

## Related documents

- [System architecture](system-overview.md)
- [Runtime execution architecture](runtime-execution.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Library architecture](library.md)
- [Track expression architecture](track-expression.md)
- [Workspace architecture](workspace.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Application shell architecture](application-shell.md)
- [Resource delivery architecture](resource-delivery.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Activity-status specification](../spec/presentation/activity-status.md) and [surface reference](../reference/presentation/activity-status.md)
- [Track-list presentation](../spec/presentation/track-presentation.md)
- [Track-column layout](../spec/presentation/track-column-layout.md)
- [Selection summary](../spec/presentation/selection-summary.md)
- [Volume control](../spec/presentation/volume-control.md)
- [Track filter](../spec/presentation/track-filter.md)
- [Metadata editing](../spec/presentation/metadata-editing.md) and [GTK track detail](../spec/linux-gtk/track-detail.md)
- [GTK dialog lifecycle](../spec/linux-gtk/dialog-lifecycle.md)
- [GTK MPRIS](../spec/linux-gtk/mpris.md) and its [surface reference](../reference/linux-gtk/mpris.md)
- [CLI execution](../spec/cli/execution.md) and [command reference](../reference/cli/command.md)
- [TUI interaction](../spec/tui/interaction.md) and [command reference](../reference/tui/command.md)
- [Application-layer review](../development/application-layer-review.md) and [UIModel organization](../development/uimodel-organization.md)
- [GTK style guide](../development/gtk-style.md)
- [RFC 0023: revision-bound metadata authoring](../rfc/0023-revision-bound-metadata-authoring.md)
- [RFC 0026: lifetime-safe GTK file-dialog callbacks](../rfc/0026-lifetime-safe-file-dialog-callbacks.md)
- [RFC 0029: versioned CLI automation protocol](../rfc/0029-versioned-cli-automation-protocol.md)
