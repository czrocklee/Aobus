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
The [layout document reference](../../reference/shell/layout-document.md), [component-state reference](../../reference/shell/layout-state.md), and [catalog reference](../../reference/shell/layout-catalog.md) own exact surfaces.

It does not define the semantic behavior of track, playback, workspace, status, or resource components.

## Code boundary

This contract spans the **UIModel** and **GTK frontend** layers in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
Platform-neutral document and state policy live under `app/include/ao/uimodel/layout/` and `app/uimodel/layout/`; GTK construction, stores, editor, and components live under `app/linux-gtk/layout/` and `app/linux-gtk/app/`.

## Terminology

- **Authored layout**: a built-in or customized `LayoutDocument` before template expansion.
- **Effective layout**: the tree obtained after template expansion.
- **Prepared layout**: a `PreparedLayout` whose authored and effective trees passed the shared limits.
- **Prepared tree**: a detached GTK component tree that has not replaced the active host generation.
- **Active shell session**: the preset id and authored layout held by `ShellLayoutSessionModel`.
- **Runtime component state**: interaction state stored separately from authored layout defaults.
- **Build generation**: one complete `LayoutHost` component tree created from one context and document.

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
- Runtime component state never rewrites authored layout unless the explicit panel-size promotion command saves its prepared layout candidate.

## State model

The shell session holds `activePresetId` and `activeLayout`.
The GTK runtime-state carrier holds the active preset id, one `LayoutComponentStateDocument`, its store, a monotonically increasing generation, edit mode, and the node-move callback.

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

`LayoutHost::prepare()` builds a detached root through `LayoutRuntime` and the `ComponentRegistry` against the next component-state generation.
`LayoutHost::commit()` advances that generation before retiring the old tree, then installs the prepared root.
Unknown component types produce a visible layout error component.
Common layout properties, declared interactions, and an optional tooltip are applied around the created component.
Nested tooltips are not built while already on a tooltip surface.

### Action binding and export

Component action props are valid only in slots permitted by the component descriptor.
Activation validates the action id, required binding context, availability, and safe anchor before calling the handler.

The Gio bridge exports registry actions to the window action map when the shell can provide any required anchor or menu context.
It initializes and later refreshes enabled state from the action registry.

### Editor

Opening the editor copies the active document and enters edit mode.
Apply rebuilds a preview without making the working document authoritative.
Cancel rebuilds the active document against a non-edit candidate view and restores the theme active when the editor opened.
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

Unknown components and template errors remain visible in the rendered layout instead of aborting the whole build.
Invalid stateful ids are diagnosed; duplicate stateful ids block editor save, while anonymous stateful nodes remain non-persistent.

Load work observes the shell lifetime stop token at executor transitions.
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

- [`ShellLayoutController.cpp`](../../../app/linux-gtk/app/ShellLayoutController.cpp) owns orchestration.
- [`ShellLayoutSessionModel.cpp`](../../../app/uimodel/layout/shell/ShellLayoutSessionModel.cpp) owns active-session policy.
- [`LayoutPreparation.cpp`](../../../app/uimodel/layout/document/LayoutPreparation.cpp) owns authored limits, bounded template expansion, and the prepared proof.
- [`LayoutRuntime.cpp`](../../../app/linux-gtk/layout/runtime/LayoutRuntime.cpp), [`ComponentRegistry.cpp`](../../../app/linux-gtk/layout/runtime/ComponentRegistry.cpp), and [`LayoutHost.cpp`](../../../app/linux-gtk/layout/runtime/LayoutHost.cpp) own GTK construction.
- [`LayoutDocument.cpp`](../../../app/uimodel/layout/document/LayoutDocument.cpp) and [`LayoutComponentState.cpp`](../../../app/uimodel/layout/component/LayoutComponentState.cpp) own explicit document/state schemas; [`LayoutStatePromoter.cpp`](../../../app/uimodel/layout/component/LayoutStatePromoter.cpp) owns reusable promotion policy.
- [`ShellLayoutStore.cpp`](../../../app/linux-gtk/app/ShellLayoutStore.cpp) and [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp) own files.

## Test map

- UIModel tests under [`test/unit/uimodel/layout/`](../../../test/unit/uimodel/layout/) protect document, bounded preparation, expansion, state, validation, promotion, and session transitions.
- [`LayoutRuntimeBuildTest.cpp`](../../../test/unit/linux-gtk/layout/components/LayoutRuntimeBuildTest.cpp), [`LayoutHostTest.cpp`](../../../test/unit/linux-gtk/layout/components/LayoutHostTest.cpp), and registry/action tests under [`test/unit/linux-gtk/layout/runtime/`](../../../test/unit/linux-gtk/layout/runtime/) protect construction and activation.
- Editor tests under [`test/unit/linux-gtk/layout/editor/`](../../../test/unit/linux-gtk/layout/editor/) protect preview, validation, save, cancel, and template editing.
- [`ShellLayoutControllerTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutControllerTest.cpp) protects failed-save retention and persistable cancel restoration across the editor/controller boundary.
- Component tests under [`test/unit/linux-gtk/layout/components/`](../../../test/unit/linux-gtk/layout/components/) protect stateful and responsive behavior.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Layout document reference](../../reference/shell/layout-document.md)
- [Layout component-state reference](../../reference/shell/layout-state.md)
- [Layout catalog and action reference](../../reference/shell/layout-catalog.md)
- [Keyboard shortcut specification](keyboard-shortcut.md)
