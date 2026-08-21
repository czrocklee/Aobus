---
id: architecture.presentation
type: architecture
status: current
domain: presentation
summary: Defines responsibility boundaries among runtime state, platform-neutral UIModel policy, and GTK, WinUI, TUI, and CLI adapters.
---
# Presentation architecture

## Scope

This document owns the boundary between frontend-neutral application state, platform-neutral presentation policy, and platform-specific adapters.
It defines what belongs in runtime, UIModel, GTK, WinUI, TUI, and CLI and how state and commands cross those boundaries.

It does not define exact layouts, widget behavior, terminal key bindings, command syntax, display strings, or editor validation rules.
Those facts belong in specifications, reference documents, and user/development guides.
It also does not own track-source membership or expression semantics; those boundaries belong to the [library](library.md) and [track expression](track-expression.md) architectures.
Workspace authority belongs to the [workspace architecture](workspace.md); interactive runtime composition, startup restoration, and frontend-specific library transition belong to the [interactive session lifecycle architecture](interactive-session-lifecycle.md).
The declarative layout document, shell session, action/component registries, GTK factory graph, component state, and shortcut adaptation belong to the [application shell architecture](application-shell.md).

## System context

Interactive presentation is a one-way dependency stack:

```text
runtime state and commands
          |
          v
platform-neutral UIModel
     /    |     \
    v     v      v
  GTK   WinUI   TUI

CLI -> runtime directly for non-interactive tasks
```

Runtime remains authoritative for state that changes application behavior across frontends.
UIModel turns that state into reusable view state and user-interaction policy.
GTK, WinUI, and TUI adapt those values to toolkit lifecycle, rendering, input,
timing, and native resources.

## Responsibilities

### Runtime presentation inputs

Runtime owns canonical identities, workspace/view lifecycle under the [workspace architecture](workspace.md), structural presentation specifications, live source/projection state, playback state, the executor-affine notification feed, and commands that mutate application behavior.
It exposes typed snapshots and subscriptions without naming widgets, CSS classes, terminal cells, or native icons.

For track-list views, runtime keeps content and shape as separate state axes.
`listId` and `filterExpression` select a source, while `TrackPresentationSpec` selects sorting, grouping, visible fields, and redundant-field suppression.
`TrackListProjection` is their composition point, not a second authority for either concern.

Runtime owns the stable ids for persisted `TrackField`, `TrackSortField`, and `TrackGroupKey` choices.
Those ids are shared by runtime workspace persistence and UIModel presentation schemas without making either layer own the other's document shape.
Authored labels are not part of the runtime definitions.

### UIModel

UIModel owns deterministic platform-neutral presentation behavior.
Its feature capsules contain view models, editor/form models, interaction models, policies, projections, formatters, catalogs, resolvers, and UI-local stores.

Shared authored copy is owned by the immutable `PresentationTextCatalog`.
It is a required-message resolver over the startup-selected `MessageCatalog` and is injected from each interactive composition root.
Fixed and caller-parameterized copy uses the canonical typed `MessageId` directly; named methods remain where UIModel maps a domain value, derives selectors, or owns an open-id fallback.
Closed inputs such as track fields, missing-value kinds, completion roles, progress kinds, and report templates resolve exhaustively.
Open backend/profile ids use their stable id as the documented fallback.
Catalog output is never persisted or parsed for recovery, ordering, grouping, aggregation, or navigation.
Borrowed catalog views point into shared immutable storage retained by `PresentationTextCatalog` copies; state crossing that lifetime owns its text.

Interactive process roots also own one leaf `MessageCatalog` selected from the operating-system locale before frontend construction.
That facade is the governed ICU localization and formatting boundary; it backs the shared UIModel semantic surface and each frontend-local slice as migration proceeds.
GTK, TUI, and WinUI retain the catalog for their process lifetime; there is no mutable process-global locale service.
Frontend-specific vocabulary stays at the leaves: GTK and TUI eagerly derive immutable typed catalogs for dense closed surfaces and use canonical typed ids through the injected shared resolver for one-to-one messages, while WinUI uses generated MRT resources selected by the same canonical locale.
Stable action names, command syntax, and shortcut tokens remain untranslated identities and enter localized patterns only as arguments.

Those roots also construct one locale-aware text-ordering policy from the
catalog's canonical requested locale. Runtime and UIModel consumers receive an
ICU-free `TextOrderingPolicy` interface; the concrete adapter remains in the
leaf `ao_app_i18n_ordering` target beside, but independent of, the message
catalog capability. Display text, locale-independent group identity, and
locale-dependent ordering keys remain separate values. Ordering keys are
transient binary data owned by the consuming projection or vocabulary
operation and never become library or session state. GTK, TUI, and WinUI retain
the policy for the lifetime of every runtime that borrows it.

The CLI constructs neither an interactive catalog nor an ordering policy. Its
target graph excludes both leaf implementations and ICU i18n, while consumers
retain their deterministic non-locale fallback when no policy is supplied. This keeps the
dependency boundary structural without moving ICU types into runtime, UIModel,
Core, or CLI.

Runtime track-group snapshots retain raw text, numeric years, empty slots, and typed missing-value kinds until UIModel resolves the three heading slots.
Runtime completion items retain query syntax, rank, and typed detail roles or frequency counts.
Core audio descriptors retain backend/profile ids and external device facts; UIModel supplies built-in backend/profile copy and semantic audio icon kinds.
`OutputDeviceViewModel` projects the same route rows and selection command for GTK, TUI, and WinUI, and exposes the exact requested route independently of engine confirmation.
`OutputDeviceSelectionPolicy` is pure: it requires non-empty backend and profile ids, rejects a profile known to be unsupported, permits an unavailable non-empty device as pending intent, and permits an empty device only when the published catalog advertises a compatible empty-id default.
Consequently an empty device with the exclusive profile is never restorable.
Runtime remains authoritative for the current accepted selection, while frontend lifecycle owners decide whether and where to persist requested intent.
That decision is carried by `OutputDeviceIntent`, which every selector surface takes by value and which has no default state: a surface either names a recorder or declares `discarded()`.
Because the type is not default-constructible, a frontend dependency bundle carrying one cannot be assembled without choosing, so a shell that fails to forward its recorder does not build.
This replaces an optional callback whose omission silently dropped every selection the user made, and it is the shape any later cross-frontend intent should take.
All three frontends record.
Shared playback reports retain a closed template and typed arguments in the notification feed, and library-task progress retains a typed operation kind plus raw subject.

The shared activity-status model consumes one immutable notification-feed update per mutation.
It derives compact and detail state from that snapshot and emits at most one render for the update; frontend adapters do not combine parallel notification signals into their own refresh policy.
Runtime-transient expiry arrives through the same canonical stream.
UIModel timers remain presentation-only for retained info and synthetic completion state, so frontend timing cannot disagree about whether a runtime notification still exists.

UIModel may subscribe to runtime services, combine several runtime snapshots, format display values, maintain an edit draft or gesture, and emit a runtime command or typed edit result.
It does not own storage transactions, playback succession, audio control, runtime retry policy, or platform lifecycle.

UIModel may resolve and complete quick-search text through runtime vocabulary ports, and may inspect a valid saved-List expression to recommend a presentation.
Those are authoring and recommendation policies: UIModel does not evaluate membership or redefine query grammar.

UIModel also owns the shared list-navigation tree projection.
It derives effective parent relationships, malformed-parent recovery, and stable sibling order once for GTK and TUI adapters.
Rows carry one unified saved-List shape; nesting is a parent relationship, not a semantic Folder, Manual, or Smart kind.

UIModel owns List-order eligibility, revision-bound authoring sessions, stable-ID movement intent, keyboard repeat suppression, and shared preference deletion cleanup.
It permits saved-order writes only for a saved List in a flat presentation with empty `sortBy`; All Tracks, grouping, active presentation sorting, source errors, maintenance, and stale bindings are explicit disabled or cancelled states.

UIModel owns semantic track-field column roles, including sizing, start/end
alignment, persisted visibility, and stored-order projection.
GTK, WinUI, and TUI translate those roles to native geometry without maintaining
independent field classifications.

UIModel owns versioned semantic schemas for its per-library column-layout and list-presentation preference state.
The schemas produce and validate platform-neutral documents; they do not choose
paths or perform frontend lifecycle saves.

UIModel owns the closed application-theme choices and their stable string ids.
Runtime persists the selected id as opaque application-preference text, while GTK maps the resolved UIModel choice to CSS classes.

Metadata and tag editors use a platform-neutral `TrackAuthoringSession`.
Session creation asks runtime to bind one exact target order to the current runtime instance and committed library revision.
The session owns that evidence and its current/invalid lifetime; it never owns a storage transaction and never silently rebinds a draft after a library change.
Maintenance, runtime replacement, any intervening effective commit, or a rejected/missing target makes the corresponding edit non-committable.
An applied submission receives evidence for the new committed revision, enabling a guarded follow-up edit or undo without weakening the original target set.

The public namespace remains `ao::uimodel`; feature ownership is expressed by singular folders mirrored across public headers, sources, and tests.

### GTK

GTK owns the desktop toolkit boundary: application/window lifetime, widgets, GObject/Gio models, CSS, dialogs, portals, MPRIS, native icons, main-context scheduling, and GTK-specific layout construction.
The interactive session lifecycle architecture owns how that platform lifetime composes and replaces the library-bound runtime; presentation owns how the live pair is adapted and rendered.

The application shell architecture refines the layout document, action/component metadata, GTK registries and builders, component state, and build context.
Presentation owns the semantic state that those shell components adapt and render.

`MainWindow` owns the visible window composition.
`MainWindowCoordinator` binds runtime/UIModel collaborators to that composition.
It also retains the shared resolver and GTK shell catalog used by menus, preferences, shortcut and presentation editors, action descriptors, and layout-component accessibility fallbacks.
Metadata/property surfaces and the Layout Editor consume that same injected resolver; stable document tokens remain identities while the GTK leaf maps built-in descriptor and enum values to localized display text.
The List preview dialog may compose read-only runtime evaluators against the const library view, but GTK cannot name committing transaction authority or call `LibraryWriter` directly.
GTK owns generation-local drag handles, drop targets, indicators, autoscroll, and native shortcut dispatch.
Rebuilding a track view destroys those gesture objects before their widgets; runtime revision and view/source generation changes invalidate retained order sessions rather than retargeting them.

### TUI

TUI owns FTXUI components, terminal geometry, key/mouse routing, overlays, refresh timing, and terminal-specific image rendering.
It constructs the same `AppRuntime`, uses shared runtime services and selected UIModel view models/policies, and builds terminal elements from their state.

TUI-local interaction models may own transient shell/overlay state but cannot become authorities for runtime playback, source order, or persisted library data.
Its output overlay consumes the same UIModel output-device view model as GTK
and WinUI. Its list chooser consumes the shared UIModel list-tree projection,
and its command palette consumes the same UIModel track-filter completer as
GTK's Quick-filter entry, while retaining terminal-only rendering, command,
and presentation routing.
The process root retains one `PresentationTextCatalog` and one `TuiTextCatalog`; terminal renderers and completion adapters borrow them, while command, key, and presentation ids remain owned by the shell interaction model or runtime.

### WinUI

WinUI owns Windows App SDK application/window lifetime, XAML resources,
dispatcher adaptation, Windows-native controls, FolderPicker, SMTC, and native
WASAPI registration. Its Modern and Classic shells consume the same runtime
playback, workspace, track-row, cover-art, quality, and Soul authorities. Shared
UIModel owns grouped display-index mapping, bounded row and artwork cache policy,
and Soul geometry and motion. WinUI owns its responsive breakpoints, XAML
rendering, HWND integration, visibility, pointer gestures, and frame scheduling.

What is this shell's own but needs no XAML to decide - its layout catalog and
dialect, its element lattice and style resolution, its themed surfaces, and its
strict settings and theme schemas - remains in the Windows-only
`aobus-winui-lib`, not shared UIModel. These sources stay separable from XAML
construction where useful, but only the native Windows suite compiles their
frontend-specific tests; naming a frontend is what keeps them out of the shared
model, not what they include.

Canonical ICU catalog sources generate WinUI's migrated shared resources as neutral `en`, `de`, and `qps-ploc` PRI candidates.
The WinUI composition root binds one explicit MRT `ResourceContext` to the same canonical tag held by `MessageCatalog` before XAML initialization, so MRT cannot independently select another UI language.
Generated C++ lookups retain canonical message keys. The build projection changes a governed single named argument to `{0}` only for native formatting and emits property-qualified aliases only for XAML `x:Uid` consumers.
Checked-in WinUI-only English resource definitions remain authority only for product names, symbolic suffixes, and internal diagnostic/protocol text. The catalog compiler merges them into the single generated neutral `en` PRI input; shared and user-facing frontend copy is generated from the canonical catalog and is not duplicated there.

### CLI

CLI is an application adapter rather than an interactive presentation layer.
It parses commands, invokes `CoreRuntime` library facilities, and serializes plain, YAML, or JSON output.
It bypasses UIModel because it does not maintain a reusable interactive view state.
Its structured automation DTOs are unversioned source-level contracts; field changes update the command reference and smoke tests in the same change.

## Boundaries and dependency direction

- Runtime has no dependency on UIModel or frontend code.
- UIModel depends on runtime interfaces and stable core value types, never platform UI libraries.
- GTK, WinUI, and TUI may depend on runtime and UIModel and own all platform resources.
- UIModel cannot include direct LMDB stores or audio player/engine/backend control headers.
- GTK, WinUI, and TUI cannot call `LibraryWriter` directly; mutations cross a UIModel editor/session or a narrow semantic runtime surface.
- A frontend adapter translates one platform event into a UIModel/runtime action and translates semantic state into platform representation.
- UIModel exposes semantic presentation kinds; GTK maps those kinds to CSS classes and native icon names at its adapter boundary.
- Core and runtime expose machine identities, structured absence, typed report/progress intent, and raw external data; shared authored copy resolves only after crossing into UIModel.
- Query syntax, persisted ids, user-authored names, metadata, paths, operating-system device descriptions, diagnostics, and command-scoped CLI output remain source data rather than catalog copy.
- Interactive localization is a leaf capability linked by GTK, TUI, and WinUI only; CLI source and final link closure exclude the catalog implementation and ICU i18n.
- Interactive locale ordering is a separate leaf capability linked by GTK,
  TUI, and WinUI only; consumers depend on the runtime policy interface and
  retain transient keys rather than locale services or ICU objects.
- Equivalent cross-frontend behavior uses the same runtime/UIModel authority instead of parallel frontend policy.
- Shared reporting presentation consumes the canonical runtime feed-update stream; GTK and TUI do not reconstruct mutation ordering from independent event types.
- List-navigation effective parents and sibling order come from one UIModel projection; GTK and TUI only adapt that tree to their native row models.
- Interactive track-filter field selection, expression classification, live-value ranking, and safe insertion are one UIModel policy shared by GTK and TUI; runtime owns only vocabulary storage mechanics.
- Presentation affects ordering, grouping, visible fields, and rendering but never changes source membership.
- Expression formatting that produces a scalar CLI string is owned by the track expression system and is not a presentation spec or UI column model.
- CLI command and output inventories remain reference concerns even though their adapter code lives at the frontend edge.

## Data and control flow

Presentation state flows outward:

```text
runtime semantic snapshot/event + raw arguments
  -> UIModel projection + PresentationTextCatalog when shared copy is needed
  -> GTK widget binding, WinUI control, or TUI render function
```

User input flows inward:

```text
GTK/WinUI/TUI input event
  -> platform event translation
  -> UIModel interaction/editor policy when needed
  -> runtime command or typed mutation request
```

Metadata/tag authoring adds an explicit revision boundary:

```text
runtime projection target ids
  -> UIModel TrackAuthoringSession binds (runtime instance, revision, exact ids)
  -> GTK/TUI edits a local value
  -> session submits a metadata/tag command with the retained binding
  -> Applied + next binding | NoOp | Stale | Unavailable
```

Purely platform concerns, such as CSS application, popover dismissal, terminal hit regions, and native file selection, stay within the frontend.
Purely structural layout concerns can travel through UIModel values, while GTK widget creation remains platform-owned.

List navigation follows a shared structural route:

```text
runtime list snapshot
  -> UIModel ListTreeProjection
  -> GTK tree nodes or TUI preorder rows
```

For a track-list view, the two independent inputs meet in runtime before presentation state flows outward:

```text
ListId + filterExpression -> TrackSource membership and saved source order
TrackPresentationSpec    -> optional sort plus projection shape
both                      -> rows and sections -> UIModel/frontend
```

A quick filter narrows the active membership while retaining the active presentation unless a separate presentation command changes it.

## Structural constraints

- UIModel values contain semantic presentation information, not platform handles or toolkit class names.
- A UIModel object can be unit-tested without a display server, terminal, storage environment, or audio backend unless it deliberately wraps a narrow runtime service.
- Frontends retain subscriptions and view models for no longer than the runtime services they observe.
- Runtime snapshots remain the source of truth after a frontend rebuilds its widget tree or terminal frame.
- Runtime/Core values never carry GTK symbolic-icon names or built-in backend marketing copy.
- Runtime grouping, completion, progress, and shared report behavior never switches on resolved catalog text.
- A UIModel notification projection consumes each non-null immutable update in callback-executor delivery order.
- UI-local persisted preferences influence presentation but do not replace canonical runtime state.
- Persisted presentation documents use explicit version gates and runtime-owned stable tokens rather than C++ enum ordinals.
- Runtime workspace, UIModel layout/preference, and GTK file ownership stay separate; sharing token conversion does not justify a universal cross-layer document schema.
- Layout component factories receive an explicit dependency bundle and runtime-state carrier rather than reaching through global frontend singletons.
- Narrow GTK evaluator composition may borrow the const core-library view; committing authority remains inaccessible to GTK and UIModel.
- An open authoring session never retargets when GTK recycles a row, selection changes, or a detail projection refreshes.
- UIModel owns binding invalidation and guarded undo policy; frontend code owns editor lifetime and rendering.

## Failure, cancellation, and lifetime boundaries

Runtime failures arrive as typed results, snapshots, or observational events.
UIModel converts semantic state into platform-neutral display or action state but does not choose runtime recovery behavior.
Frontends decide how and where to render an error and own cancellation tied to widget/dialog/terminal lifetime.

`TrackAuthoringSession` observes authoring availability and invalidates itself when its runtime instance/revision is no longer current.
Runtime revalidates the same facts under writer ownership at submission, so delayed availability delivery cannot permit a stale commit.

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
- [`PresentationTextCatalog`](../../app/include/ao/uimodel/presentation/PresentationTextCatalog.h) owns shared authored copy and open-id fallback.
- [`OutputDeviceIntent`](../../app/include/ao/uimodel/playback/output/OutputDeviceIntent.h) owns the typed destination for a requested route and its explicit absence.
- [`OutputDeviceViewModel`](../../app/include/ao/uimodel/playback/output/OutputDeviceViewModel.h)
  owns shared output-route projection, selection commands, and requested-intent reporting;
  [`OutputDeviceSelectionPolicy`](../../app/include/ao/uimodel/playback/output/OutputDeviceSelectionPolicy.h)
  owns pure persisted-selection admission and fallback resolution.
- [`TrackGroupHeadingPresentation`](../../app/include/ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h) resolves structured runtime group headings.
- [`TrackAuthoringSession`](../../app/include/ao/uimodel/library/property/TrackAuthoringSession.h) owns revision-bound metadata/tag interaction lifetime.
- [`TrackField`](../../app/include/ao/rt/TrackField.h) owns stable field, sort, and group token conversion.
- [`TrackColumnLayoutYamlSchema`](../../app/include/ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h) and [`ListPresentationPreferenceYamlSchema`](../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h) own versioned UIModel presentation documents.
- [`ListTreeProjection`](../../app/include/ao/uimodel/library/list/ListTreeProjection.h) owns shared list-navigation hierarchy and recovery policy.
- [`ThemePreset`](../../app/include/ao/uimodel/preference/ThemePreset.h) owns semantic application-theme choices and stable-id resolution.
- [`MainWindow`](../../app/linux-gtk/app/MainWindow.h), [`MainWindowCoordinator`](../../app/linux-gtk/app/MainWindowCoordinator.h), and [`GtkUiDependencies`](../../app/linux-gtk/app/GtkUiDependencies.h) define GTK composition boundaries.
- [`MainContextCallbackScope`](../../app/linux-gtk/common/MainContextCallbackScope.h) bounds GTK-main-context callbacks to their owner lifetime.
- [`LayoutRuntime`](../../app/linux-gtk/layout/runtime/LayoutRuntime.h) and [`LayoutBuildContext`](../../app/linux-gtk/layout/runtime/LayoutBuildContext.h) build GTK layout values into widgets.
- [`app/tui/App.cpp`](../../app/tui/App.cpp) composes runtime, selected UIModel objects, terminal controllers, and rendering.
- [`CliRuntime`](../../app/cli/CliRuntime.h) is the non-interactive adapter boundary.
- [`aobus-winui-lib`](../../app/windows-winui/CMakeLists.txt), [`MainWindow`](../../app/windows-winui/MainWindow.xaml), [`ShellBuilder`](../../app/windows-winui/layout/ShellBuilder.h), [`TrackListController`](../../app/windows-winui/track/TrackListController.h), [`TrackItemView`](../../app/windows-winui/track/TrackItemView.h), [`StringResources`](../../app/windows-winui/platform/StringResources.h), and [`AobusSoulControl`](../../app/windows-winui/playback/AobusSoulControl.h) define WinUI presentation adaptation.
- [`MessageCatalog`](../../app/include/ao/i18n/MessageCatalog.h), its [`ICU implementation`](../../app/i18n/MessageCatalog.cpp), and the canonical [`catalog assets`](../../app/i18n/catalog/root.txt) define the interactive localization leaf.
- [`GtkTextCatalog`](../../app/linux-gtk/i18n/GtkTextCatalog.h), [`TuiTextCatalog`](../../app/tui/TuiTextCatalog.h), direct typed-id call sites, and [`WinUiResourceProjection`](../../app/i18n/WinUiResourceProjection.h) define frontend-owned copy and native projection boundaries without adding forwarding getters or frontend enums for one-to-one messages.
- [`AssertUimodelOrganization.cmake`](../../cmake/AssertUimodelOrganization.cmake) and [`AssertNoForbiddenIncludes.cmake`](../../cmake/AssertNoForbiddenIncludes.cmake) enforce organization, dependency, and platform-vocabulary constraints.

## Test map

- [`test/unit/uimodel/`](../../test/unit/uimodel) mirrors UIModel feature capsules and protects platform-neutral policy.
- [`TrackAuthoringSessionTest.cpp`](../../test/unit/uimodel/library/property/TrackAuthoringSessionTest.cpp) protects binding invalidation, all-or-none results, and guarded follow-up submissions.
- [`TrackFieldTest.cpp`](../../test/unit/runtime/TrackFieldTest.cpp) and UIModel presentation schema tests protect stable persistence vocabulary and semantic document validation.
- [`PresentationTextCatalogTest.cpp`](../../test/unit/uimodel/presentation/PresentationTextCatalogTest.cpp) protects catalog completeness, structured formatting, and open-id fallback.
- [`MessageCatalogTest.cpp`](../../test/unit/i18n/MessageCatalogTest.cpp), [`CatalogPatternTest.cpp`](../../test/unit/i18n/CatalogPatternTest.cpp), and the native [`WinUiLocalizationProbe.cpp`](../../test/helper/WinUiLocalizationProbe.cpp) protect explicit fallback, format signatures, immutable concurrent use, deterministic generation, and ICU/MRT parity.
- [`MenuControllerTest.cpp`](../../test/unit/linux-gtk/app/MenuControllerTest.cpp), [`PreferencesWindowTest.cpp`](../../test/unit/linux-gtk/preference/PreferencesWindowTest.cpp), [`ShortcutEditorWidgetTest.cpp`](../../test/unit/linux-gtk/preference/ShortcutEditorWidgetTest.cpp), [`TrackCustomViewDialogTest.cpp`](../../test/unit/linux-gtk/track/TrackCustomViewDialogTest.cpp), [`LayoutEditorTextTest.cpp`](../../test/unit/linux-gtk/layout/editor/LayoutEditorTextTest.cpp), and [`RenderTest.cpp`](../../test/unit/tui/RenderTest.cpp) protect the migrated GTK/TUI surface, localized built-in layout vocabulary, and preservation of command/key/document identities.
- [`OutputDeviceIntentTest.cpp`](../../test/unit/uimodel/playback/output/OutputDeviceIntentTest.cpp) protects the undecided-destination compile barrier and recorder dispatch.
- [`OutputDeviceViewModelTest.cpp`](../../test/unit/uimodel/playback/output/OutputDeviceViewModelTest.cpp)
  and [`OutputDeviceSelectionPolicyTest.cpp`](../../test/unit/uimodel/playback/output/OutputDeviceSelectionPolicyTest.cpp)
  protect the three-frontend selector projection, command route, and restore
  admission policy.
- [`ListTreeProjectionTest.cpp`](../../test/unit/uimodel/library/list/ListTreeProjectionTest.cpp) protects shared list hierarchy, recovery, and ordering.
- [`ThemePresetTest.cpp`](../../test/unit/uimodel/preference/ThemePresetTest.cpp) protects theme-id resolution and fallback.
- [`MainWindowCoordinatorTest.cpp`](../../test/unit/linux-gtk/app/MainWindowCoordinatorTest.cpp) and [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) protect GTK composition.
- [`MainContextCallbackScopeTest.cpp`](../../test/unit/linux-gtk/common/MainContextCallbackScopeTest.cpp) protects callback invalidation and teardown ordering.
- [`ImportExportCoordinatorTest.cpp`](../../test/unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp) protects native chooser policy, handoff, and export-mode response invalidation.
- [`ShortcutEditorWidgetTest.cpp`](../../test/unit/linux-gtk/preference/ShortcutEditorWidgetTest.cpp) protects delayed conflict-response invalidation.
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
- [Presentation text catalog reference](../reference/presentation/text-catalog.md)
- [Interactive localization specification](../spec/presentation/localization.md)
- [Track-list presentation](../spec/presentation/track-presentation.md)
- [List-navigation tree](../spec/presentation/list-tree.md)
- [Track-column layout](../spec/presentation/track-column-layout.md)
- [Persisted presentation state](../reference/presentation/persisted-state.md)
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
