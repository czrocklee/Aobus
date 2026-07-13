---
id: shell.layout-lifecycle
type: spec
status: current
domain: application-shell
summary: Defines preset selection, layout loading, template expansion, GTK construction, editor rebuilds, component state, and shell teardown.
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
- **Active shell session**: the preset id and authored layout held by `ShellLayoutSessionModel`.
- **Runtime component state**: interaction state stored separately from authored layout defaults.
- **Build generation**: one complete `LayoutHost` component tree created from one context and document.

## Invariants

- The active preset is `classic` or `modern`; an empty or unknown requested preset falls back to `classic`.
- A customized preset file overrides its matching built-in document; absence uses the built-in document.
- Component state is selected by the same preset id as the authored document.
- UIModel template expansion completes before the GTK registry creates the root component.
- One host owns at most one active component tree.
- A rebuild replaces the complete tree and does not mutate runtime/domain authorities represented by its components.
- Action handlers and availability come from the live action registry, not from layout YAML or keymap data.
- Runtime component state never rewrites authored layout unless the explicit panel-size promotion command succeeds through its current best-effort store path.

## State model

The shell session holds `activePresetId` and `activeLayout`.
The GTK runtime-state carrier holds the active preset id, one `LayoutComponentStateDocument`, its store, a monotonically increasing generation, edit mode, and the node-move callback.

The controller is either loading, displaying one active generation, previewing an editor working document, applying an editor save, resetting state, promoting state, or tearing down.
These are orchestration phases rather than a published enum.

## Commands and transitions

### Load

`loadLayout()` starts a lifetime-bound asynchronous workflow.
On the worker it loads application preferences, selects a supported preset, loads customized YAML or the built-in preset, and loads matching component state or constructs an empty state document.
It resumes on the callback executor before installing the session, diagnosing stateful ids, or touching GTK.

Cancellation before callback resumption installs nothing.
An internal exception is logged and leaves the previous shell generation unchanged.

### Expand and build

A `template` node requires `props.templateId`.
Expansion recursively replaces it with the referenced template, overlays a non-empty reference id, overlays reference layout values and non-`templateId` props, appends reference children, and replaces the tooltip when the reference supplies one.
Missing, unknown, or recursive references produce an error node.

`LayoutHost::setLayout()` builds a new root through `LayoutRuntime` and the `ComponentRegistry`, then replaces the previously active component.
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
Cancel rebuilds the active document and restores the theme active when the editor opened.

Save writes modified preset documents, removes reset customizations, prunes or removes matching component state, installs the selected active document, reloads its matching runtime state, updates the selected preset preference, restores the persisted application theme, and rebuilds.
The current store calls are best-effort; failures can be logged while the in-memory session proceeds.

### Runtime-state reset and promotion

Reset removes the active preset's state file, installs an empty state document, and rebuilds from authored defaults.
It does not modify customized or built-in layout YAML.

Panel-size promotion first prepares a copy.
For `split`, `positionPercent` is clamped to `[0, 1]`, written as `initialPositionPercent`, and removes authored `position`.
For `collapsibleSplit`, `size` is clamped to at least `50`, written as `position`, and removes authored `initialPositionPercent`.
Promoted keys leave runtime state; residual keys retain a baseline hash recomputed against the new authored node.
The controller asks for confirmation before writing and installing the promoted copies.

## Failure and cancellation

Malformed or absent custom layout files fall back to the built-in preset through `ShellLayoutStore::load()`.
Malformed, mismatched, absent, or unsupported component-state documents fall back to empty state.
Preset ids reject empty values, path separators, and `..`; component-state ids also reject NUL.

Unknown components and template errors remain visible in the rendered layout instead of aborting the whole build.
Invalid stateful ids are diagnosed; duplicate stateful ids block editor save, while anonymous stateful nodes remain non-persistent.

Load work observes the shell lifetime stop token at executor transitions.
Component construction and rebuild are callback-executor GTK operations and have no independent cancellation point.
Current layout save and several state mutations report only through logging or booleans; RFC 0015 proposes coherent result-bearing persistence transactions.

## Persistence and versioning

The selected preset belongs to global application preferences.
Customized layouts use one YAML file per preset under the layout configuration directory.
Component runtime state uses one YAML file per preset under the state directory.

Layout documents and component-state documents currently use version `1`.
An unsupported component-state document or entry version is ignored rather than migrated.
Authored layout documents currently do not reject an unsupported numeric version and have no shared file/model/expansion budget; [RFC 0025](../../rfc/0025-bounded-shell-layout-documents.md) proposes the strict bounded candidate behavior.
Exact fields and managed locations belong to reference.

## Frontend observations

Users observe the newly built shell only after callback-executor installation.
Layout errors appear as visible red diagnostic components.
Editor preview changes the live shell temporarily; cancel and save restore a coherent active generation according to the transitions above.

Stateful split and collapsible-split interactions update runtime component state without changing the authored document.
GTK responsive and component-specific behavior remains owned by the individual component implementation and focused specifications where present.

## Implementation map

- [`ShellLayoutController.cpp`](../../../app/linux-gtk/app/ShellLayoutController.cpp) owns orchestration.
- [`ShellLayoutSessionModel.cpp`](../../../app/uimodel/layout/shell/ShellLayoutSessionModel.cpp) owns active-session policy.
- [`LayoutTemplateExpansion.cpp`](../../../app/uimodel/layout/document/LayoutTemplateExpansion.cpp) owns template behavior.
- [`LayoutRuntime.cpp`](../../../app/linux-gtk/layout/runtime/LayoutRuntime.cpp), [`ComponentRegistry.cpp`](../../../app/linux-gtk/layout/runtime/ComponentRegistry.cpp), and [`LayoutHost.cpp`](../../../app/linux-gtk/layout/runtime/LayoutHost.cpp) own GTK construction.
- [`LayoutComponentState.cpp`](../../../app/uimodel/layout/component/LayoutComponentState.cpp) and [`LayoutStatePromoter.cpp`](../../../app/uimodel/layout/component/LayoutStatePromoter.cpp) own reusable state policy.
- [`ShellLayoutStore.cpp`](../../../app/linux-gtk/app/ShellLayoutStore.cpp) and [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp) own files.

## Test map

- UIModel tests under [`test/unit/uimodel/layout/`](../../../test/unit/uimodel/layout/) protect document, expansion, state, validation, promotion, and session transitions.
- [`LayoutRuntimeBuildTest.cpp`](../../../test/unit/linux-gtk/layout/components/LayoutRuntimeBuildTest.cpp), [`LayoutHostTest.cpp`](../../../test/unit/linux-gtk/layout/components/LayoutHostTest.cpp), and registry/action tests under [`test/unit/linux-gtk/layout/runtime/`](../../../test/unit/linux-gtk/layout/runtime/) protect construction and activation.
- Editor tests under [`test/unit/linux-gtk/layout/editor/`](../../../test/unit/linux-gtk/layout/editor/) protect preview, validation, save, cancel, and template editing.
- Component tests under [`test/unit/linux-gtk/layout/components/`](../../../test/unit/linux-gtk/layout/components/) protect stateful and responsive behavior.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Layout document reference](../../reference/shell/layout-document.md)
- [Layout component-state reference](../../reference/shell/layout-state.md)
- [Layout catalog and action reference](../../reference/shell/layout-catalog.md)
- [Keyboard shortcut specification](keyboard-shortcut.md)
- [RFC 0025: bounded shell layout documents](../../rfc/0025-bounded-shell-layout-documents.md)
