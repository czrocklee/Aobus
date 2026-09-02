---
id: shell.layout-lifecycle
type: spec
status: current
domain: application-shell
summary: Defines bounded layout preparation, preset loading, staged GTK construction, editor rebuilds, component state, and shell teardown.
---
# Shell layout lifecycle

## Scope

This specification defines current behavior from GTK shell preset selection through layout loading, widget construction, action binding, editor preview/save, component-state restoration and promotion, rebuild, and teardown.
The [layout document reference](../../reference/shell/layout-document.md), [component-state reference](../../reference/shell/layout-state.md), and [GTK schema reference](../../reference/shell/layout-schema.md) own exact surfaces.

It does not define the semantic behavior of track, playback, workspace, status, or resource components.

## Code boundary

This contract spans the **UIModel** and **GTK frontend** layers in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
Platform-neutral document and state policy live under `app/include/ao/uimodel/layout/` and `app/uimodel/layout/`; GTK construction, stores, editor, and components live under `app/linux-gtk/layout/` and `app/linux-gtk/app/`.

## Terminology

- **Authored layout**: a built-in or customized `LayoutDocument` before template expansion.
- **Effective layout**: the tree obtained after template expansion.
- **Prepared layout**: a `PreparedLayout` whose authored and effective trees passed the shared limits.
- **Prepared tree**: a detached GTK component tree that has not replaced the active host generation.
- **Active shell session**: the preset id, authored layout, component state, edit state, and committed generation held by `LayoutSession`.
- **Build snapshot**: immutable, owning candidate inputs captured by `LayoutSession` for one tree traversal.
- **Component-state binding**: one node's restored state and generation-fenced write authority.
- **Runtime component state**: interaction state stored separately from authored layout defaults.
- **Build generation**: one complete `LayoutHost` component tree created from one context and document.
- **Action registration**: one scoped owner of the exact Gio actions and activation connections installed into an action map or attached group.

## Invariants

- The active preset is `classic` or `modern`; an empty or unknown requested preset falls back to `classic`.
- A customized preset file overrides its matching built-in document; absence uses the built-in document.
- Component state is selected by the same preset id as the authored document.
- Every GTK root is built from a `PreparedLayout`; raw authored documents cannot enter `LayoutRuntime`.
- UIModel bounded template expansion completes before the GTK registry creates the root component.
- One host owns at most one active component tree.
- A rebuild prepares a complete detached tree before committing a replacement.
- Failed document preparation or GTK construction leaves the active session, tree, component state, and generation unchanged.
- Action handlers and availability come from the live action registry, not from layout YAML or keymap data.
- Every Gio action-map export or attached action group has a registration that retires before both the map/group and every callback target.
- Registration retirement disconnects all activation handlers before removing actions, and removes an id only when the map still contains the exact registered action.
- An independently retained action becomes inert after registration retirement, while a newer same-id replacement remains active.
- `ActionActivationContext` and its window and anchor references are valid only for the synchronous activation or availability call.
- Runtime component state never rewrites authored layout unless the explicit panel-size promotion command saves its prepared layout candidate.

## State model

`LayoutSession` is the window-lifetime authority for the active preset, authored document, component-state document, state store, monotonically increasing generation, edit mode, and node-move callback.
It captures those candidate inputs into an owning `LayoutBuildSnapshot`; a build never switches between a borrowed live view and an explicit candidate representation.
Each persistent component retains a `ComponentStateBinding` produced from that snapshot.
The binding may write only while its captured generation is the committed session generation and the build was for the main surface, outside edit mode, with a stable node id, active preset, and state store.

The controller is either loading, displaying one active generation, previewing an editor working document, applying an editor save, resetting state, promoting state, or tearing down.
These are orchestration phases rather than a published enum.

## Commands and transitions

### Load

`loadLayout()` starts a lifetime-bound asynchronous workflow.
On the worker it loads application preferences and selects a supported preset.
A missing custom file selects the matching built-in document; a rejected custom file logs its bounded error, remains untouched, and also selects the matching built-in document.
The selected authored document is prepared before the worker returns, and matching component state is loaded or replaced by an empty state document.

The workflow resumes on the callback executor, prepares a detached GTK tree against the candidate preset and component state, and only then installs the session/state and commits the tree.

Cancellation before callback resumption installs nothing.
An internal exception is logged and leaves the previous shell generation unchanged.

### Expand and build

A `template` node requires `props.templateId`.
Preparation recursively replaces it with the referenced template, overlays a non-empty reference id, overlays reference layout values and non-`templateId` props, appends reference children, and replaces the tooltip when the reference supplies one.
Missing, unknown, or recursive references produce a bounded error node.
Authored and produced entries, owned string bytes, and depth are charged against the limits in the [layout document reference](../../reference/shell/layout-document.md).

`LayoutHost::prepare()` builds a detached root through `LayoutRuntime` and the `ComponentRegistry` against a `LayoutBuildSnapshot` for the next component-state generation.
The controller applies or advances that generation before `LayoutHost::commit()` retires the old tree, so teardown from the predecessor cannot write into successor state.
The host then installs the prepared root.
`TrackPageHost::stack()` is a shell-owned singleton that may occur as either `track.table` or `workspace.withDetailPane`.
Candidate construction transfers it through a `SharedWidgetHandoff`: the widget has one GTK parent throughout a successful switch, and discarding or failing the candidate restores its exact previous parent before control returns.
Unknown component types produce a visible layout error component.
Common layout properties, declared interactions, and an optional tooltip are applied around the created component.
Authored common properties are applied after construction, so a component that also drives one of those properties at runtime reconciles both sources once they are applied.
Authored visibility and component-managed visibility both have to admit a widget before it is shown, which keeps an authored value from overriding a component's own visibility contract in either direction.
Nested tooltips are not built while already on a tooltip surface.
A pointer entering a component schedules a reveal only when its tooltip root is visible at that time, using the build context's timeout scheduler and the GTK main-context scheduler when no override is supplied.
The delayed callback rechecks visibility before revealing the tooltip.
Hiding the tooltip root cancels a pending reveal and closes an open tooltip immediately.
Content that becomes visible under a stationary pointer schedules no new reveal; the pointer must leave and enter again.

### Action binding and export

Component action props are valid only in slots permitted by the component schema.
Activation validates the action id, required binding context, availability, and safe anchor before calling the handler.

The Gio bridge exports registry actions to the window action map when the shell can provide any required anchor or menu context.
It constructs the scoped registration before installing the first action, initializes enabled state from the action registry, and later refreshes only exact actions that remain installed by that session.
Replacing a bridge session retires the old registration first.
Ending a session disconnects activation handlers before exact-identity removal, including after partial export failure.
Window, List-navigation, application, Layout Editor, and private Tag-edit actions use the same scoped or explicit close boundary when their map or attached group can outlive the callback producer.

### Editor

Opening the editor copies the active document and marks the `LayoutSession` as editing.
Apply rebuilds a preview from an edit-mode snapshot without making the working document authoritative.
Cancel rebuilds the active document from a non-edit snapshot and restores the theme active when the editor opened.
The restored tree resumes normal component-state persistence and does not retain editor-only gestures.

Save prepares every modified document and the active document, prunes a candidate active component-state document, and prepares the active GTK tree before persistence begins.
It then writes modified preset documents, removes reset customizations, prunes or removes matching component state, updates the selected preset preference, restores the persisted application theme, and commits the selected session/state/tree.
An individual layout save or remove failure aborts the in-memory installation, reports the error, and leaves the editor open with its draft so the user may retry.
Earlier preset-file operations in the same multi-preset request are not rolled back; repeating those completed saves is idempotent.
The [GTK dialog-lifecycle specification](../linux-gtk/dialog-lifecycle.md) owns the editor's visible close and error-message behavior.

### Runtime-state reset and promotion

Reset prepares the authored layout and a detached tree against empty candidate state before removing the active preset's state file, installing the empty state document, and committing the tree.
It does not modify customized or built-in layout YAML.

Panel-size promotion first prepares a copy.
For `split`, `positionPercent` is clamped to `[0, 1]`, written as `initialPositionPercent`, and removes authored `position`.
For `collapsibleSplit`, `size` is clamped to at least `50`, written as `position`, and removes authored `initialPositionPercent`.
Promoted keys leave runtime state; residual keys retain a baseline hash recomputed against the new authored node.
The controller asks for confirmation, prepares the promoted layout and GTK tree, and requires the layout save to succeed before writing remaining component state and installing the promoted copies.

## Failure and cancellation

An absent custom layout file selects the built-in preset normally.
Malformed, unsupported-version, or over-budget custom layout files return a typed rejection, remain byte-identical, and fall back to the matching prepared built-in preset.
Malformed, mismatched, absent, or unsupported component-state documents are rejected and fall back to empty state.
Preset ids reject empty values, path separators, and `..`; component-state ids also reject NUL.

Validated shell candidates reject unknown components; direct component-runtime probes may still construct the visible diagnostic placeholder used for defensive fallback.
Template-expansion errors remain bounded preparation failures.
Invalid stateful ids are diagnosed; duplicate stateful ids block editor save, while anonymous stateful nodes remain non-persistent.

Load work observes the shell lifetime stop token at executor transitions.
Shell teardown closes callback admission and action-state subscriptions before retiring Gio actions, so no teardown-time state publication can refresh a retired export.
Component construction and rebuild are callback-executor GTK operations and have no independent cancellation point.
Preparation, layout load/save/remove, and detached GTK construction return existing typed `Error` values.
The layout store preserves its prior live document and backing bytes on a returned failure; component-state operations retain their existing optional/Boolean reporting contract.
The shell does not add generic commit receipts or a blocked-store recovery mode.

## Persistence and versioning

The selected preset belongs to global application preferences.
Customized layouts use one YAML file per preset under the layout configuration directory.
Component runtime state uses one YAML file per preset under the state directory.

Layout documents and component-state documents currently use version `1`.
Their explicit schemas reject unsupported document and entry versions before interpreting version-specific payload; neither format has a legacy or migration fallback.
Customized layout files and both authored/effective trees use the exact default budgets in the [layout document reference](../../reference/shell/layout-document.md).
There is no automatic migration, quarantine, or rewrite of a rejected custom file.
Exact fields and managed locations belong to reference.

## Frontend observations

Users observe a replacement shell only after detached construction succeeds and the callback executor commits it.
Layout errors appear as visible red diagnostic components.
Editor preview changes the live shell temporarily; cancel and save restore a coherent active generation according to the transitions above.

Stateful split and collapsible-split interactions update runtime component state without changing the authored document.
GTK responsive and component-specific behavior remains owned by the individual component implementation and focused specifications where present.

## Implementation map

- [`ShellLayoutController.cpp`](../../../app/linux-gtk/app/ShellLayoutController.cpp) owns orchestration; [`GioActionBridge.cpp`](../../../app/linux-gtk/layout/runtime/GioActionBridge.cpp) and [`ActionMapRegistration.cpp`](../../../app/linux-gtk/common/ActionMapRegistration.cpp) own scoped export, disconnection, and exact-identity removal.
- [`LayoutSession.cpp`](../../../app/uimodel/layout/shell/LayoutSession.cpp) owns session state, immutable build snapshots, generation advancement, component-state bindings, and panel-size promotion preparation.
- [`LayoutPreparation.cpp`](../../../app/uimodel/layout/document/LayoutPreparation.cpp) owns authored limits, bounded template expansion, and the prepared proof.
- [`LayoutRuntime.cpp`](../../../app/linux-gtk/layout/runtime/LayoutRuntime.cpp), [`ComponentRegistry.cpp`](../../../app/linux-gtk/layout/runtime/ComponentRegistry.cpp), and [`LayoutHost.cpp`](../../../app/linux-gtk/layout/runtime/LayoutHost.cpp) own GTK construction; [`ComponentTooltipController.cpp`](../../../app/linux-gtk/layout/runtime/ComponentTooltipController.cpp) owns tooltip scheduling and visible-content lifetime.
- [`LayoutDocument.cpp`](../../../app/uimodel/layout/document/LayoutDocument.cpp) and [`LayoutComponentState.cpp`](../../../app/uimodel/layout/component/LayoutComponentState.cpp) own explicit document/state schemas; [`LayoutStatePromoter.cpp`](../../../app/uimodel/layout/component/LayoutStatePromoter.cpp) owns reusable promotion policy.
- [`LayoutSchema.cpp`](../../../app/uimodel/layout/component/LayoutSchema.cpp) owns component/action schema lookup and action-slot resolution; `LayoutSession.cpp` owns component-state resolution and write guards.
- [`ShellLayoutStore.cpp`](../../../app/linux-gtk/app/ShellLayoutStore.cpp) and [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp) own files.

## Test map

- UIModel tests under [`test/unit/uimodel/layout/`](../../../test/unit/uimodel/layout/) protect document preparation, state, schema validation, action-slot resolution, promotion, session transitions, immutable snapshots, and generation-fenced writes.
- [`LayoutRuntimeBuildTest.cpp`](../../../test/unit/linux-gtk/layout/components/LayoutRuntimeBuildTest.cpp), [`LayoutHostTest.cpp`](../../../test/unit/linux-gtk/layout/components/LayoutHostTest.cpp), and registry/action tests under [`test/unit/linux-gtk/layout/runtime/`](../../../test/unit/linux-gtk/layout/runtime/) protect construction, activation, shared-widget switching, and failed-handoff rollback.
- Editor tests under [`test/unit/linux-gtk/layout/editor/`](../../../test/unit/linux-gtk/layout/editor/) protect preview, validation, save, cancel, and template editing.
- [`ShellLayoutControllerTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutControllerTest.cpp) protects failed-save retention, persistable cancel restoration, repeated action export, and controller teardown across the editor/controller boundary.
- [`GioActionBridgeTest.cpp`](../../../test/unit/linux-gtk/layout/runtime/GioActionBridgeTest.cpp), [`ActionMapRegistrationTest.cpp`](../../../test/unit/linux-gtk/common/ActionMapRegistrationTest.cpp), and [`MainWindowTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowTest.cpp) protect retained-action revocation, partial-export rollback, exact replacement identity, and window action teardown.
- Component tests under [`test/unit/linux-gtk/layout/components/`](../../../test/unit/linux-gtk/layout/components/) protect stateful and responsive behavior; [`PlaybackImageTest.cpp`](../../../test/unit/linux-gtk/layout/components/PlaybackImageTest.cpp) protects tooltip visibility gating and delayed-reveal eligibility.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Layout document reference](../../reference/shell/layout-document.md)
- [Layout component-state reference](../../reference/shell/layout-state.md)
- [GTK layout schema and action reference](../../reference/shell/layout-schema.md)
- [Keyboard shortcut specification](keyboard-shortcut.md)
