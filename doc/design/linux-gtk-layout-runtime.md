# Linux/GTK Layout Runtime

How a layout document becomes a live GTK widget tree, and the three
collaborator / state / carrier pieces a build borrows. This describes current
implemented behavior. The persisted component-state semantics live in
`layout-component-state.md`; this document covers the runtime carrier
architecture around it.

## Build pipeline

A `LayoutDocument` is turned into widgets by `LayoutHost::setLayout` /
`LayoutRuntime::build`, which walk the node tree and call a `ComponentFactory`
per node through `ComponentRegistry`. The factory signature is:

```cpp
using ComponentFactory =
  std::unique_ptr<LayoutComponent> (*)(LayoutBuildContext&, uimodel::LayoutNode const&);
```

`ShellLayoutController` drives every (re)build through `rebuildHost`, which
assembles a **fresh** `LayoutBuildContext` for that build and passes it to the
host. The carrier is never held as a long-lived member.

## The three pieces

| Type | Role | Owner | Lifetime | Retained by |
| --- | --- | --- | --- | --- |
| `GtkUiDependencies` | `*Dependencies` | none (consumed during shell construction) | construction | — |
| `LayoutRuntimeState` | `*State` | `ShellLayoutController` | shell | `StatefulComponentState` |
| `LayoutBuildContext` | `*Context` | none (assembled per build) | one build | — |

### `GtkUiDependencies` (`app/linux-gtk/app/GtkUiDependencies.h`)

The single source of the borrowed collaborators the GTK application layer hands
to layout components: track page host, playback sequence and command surface,
image cache, tag edit controller, track presentation catalog and preferences,
list navigation controller, theme coordinator, import/export actions, the
smart-list creation callback, and the shell menu model.

Produced by `MainWindowCoordinator::uiDependencies()` and consumed by the
`ShellLayoutController` constructor. The controller unpacks the collaborators
it retains instead of retaining the carrier. `ShellLayoutController::setMenuModel()`
supplies the menu model separately because `MainWindow` builds it later.

For each layout build, the controller assembles a fresh read-only dependency
view from those explicit members. Components read the specific collaborator
they need as `ctx.dependencies.<field>`; neither the construction bundle nor
the per-build view is retained.

### `LayoutRuntimeState` (`app/linux-gtk/layout/runtime/LayoutRuntimeState.h`)

Mutable state that lives for the shell's whole lifetime: `activePresetId`, the
`componentState` document and its `componentStateStore`, the
`componentStateGeneration` write-guard, and the edit-mode pair (`editMode` +
`onNodeMoved`).

Owned by `ShellLayoutController`. `StatefulComponentState` retains a pointer to
it (not to the transient carrier) so a component can persist its runtime state
after the build call returns. The generation guard is bumped on each layout swap
so a stale component destructing during reset/load/save-defaults cannot pollute a
freshly assigned document. See `layout-component-state.md` for the persisted
state-document semantics.

During shell teardown, the controller cancels its layout workflows and explicitly
clears `LayoutHost` while the runtime state and component-state store are still
alive. Unlike a layout swap, this final clear does not advance the generation, so
the current tree may flush pending state before its dependencies are released.

### `LayoutBuildContext` (`app/linux-gtk/layout/runtime/LayoutBuildContext.h`)

The passive per-build carrier. Assembled fresh for each build and discarded when
it returns; it owns nothing and is not retained as wiring. Its top-level fields
are grouped by kind:

- **build environment**: `surface`, `registry`, `actionRegistry`, `runtime`,
  `parentWindow`;
- **borrowed state**: `LayoutRuntimeState& runtimeState`;
- **borrowed wiring**: `GtkUiDependencies const& dependencies`;
- **build-traversal scope**: `detailScope` / `detailUndo`, saved and restored by
  `TrackDetailScope` during the recursion; `surface` is flipped to `Tooltip` by
  the tooltip `SurfaceGuard` for tooltip subtrees.

This satisfies the strict `*Context` contract from
`doc/dev/naming-conventions.md`: assembled for one bounded operation, borrows its
collaborators, and holds no ownership, lifecycle, or retained domain state.

## Why the split

A single bundle previously conflated construction wiring, mutable runtime state,
and the per-build environment, and was both held long-lived and retained by
components — so its `*Context` name was inaccurate. Separating the three roles
lets each name tell the truth: `StatefulComponentState` retains only the state
it writes, the wiring has one owner and one name per collaborator, and the
carrier is genuinely per-build.
