---
id: shell.layout-adaptation
type: spec
status: current
domain: application-shell
summary: Defines GTK allocation-driven responsiveness, raster targets, responsive classes, collapsible splits, and component-state interaction.
---
# Shell layout-adaptation specification

## Scope

This specification owns GTK-specific adaptation of declarative shell structure to live widget allocation.
It defines logical sizing, responsive CSS buckets, image render targets, fixed-slot guidance, and the collapsible split interaction.
Exact component property names and defaults belong to the [layout catalog reference](../../reference/shell/layout-catalog.md).

## Code boundary

The neutral layout document carries structure and property values.
GTK components under `app/linux-gtk/layout/component/container/` observe allocation, construct widgets, apply CSS classes, and adapt pointer gestures.
`ImageWidget` owns GTK raster-target selection.
UIModel component-state values own portable state guards and promotion, not GTK measurement.

## Terminology

- **Logical allocation** is the width/height GTK assigns in logical pixels.
- A **responsive bucket** is compact, regular, or wide according to one observed allocation axis.
- The **collapsible pane size** is the remembered size of the collapsible child, not a `Gtk::Paned` divider coordinate.
- A **manual size** is chosen by drag and supersedes a percentage default.
- A **baseline guard** ties restored component state to the authored node configuration it was derived from.

## Invariants

- Structural layout uses logical widget allocation, never physical monitor pixels or display scale.
- Display scale influences raster fidelity only.
- CSS owns visual sizing and affordances; the layout document owns structure and explicit intent; C++ owns intrinsic/allocation-driven behavior.
- `responsiveClass` has exactly one child and applies exactly one current bucket class.
- `collapsibleSplit` has exactly two children and supports one start or end collapsible side.
- The collapsible pane can reach a true hidden state without leaving `Gtk::Paned` allocation behind.
- Child minimum-size changes do not overwrite the user's remembered pane size.
- Dragging below the minimum clamps the revealed pane; hiding uses reveal state rather than persisting a zero pane size.
- Component state is written only by the current build generation and restored only when type, version, id, and baseline match.

## State model

`responsiveClass` retains its axis, normalized breakpoints, class prefix, and current bucket.
The default breakpoints are 820 logical pixels for compact maximum and 1180 for regular maximum; an authored regular maximum below compact is normalized upward.

`collapsibleSplit` retains current size, reveal state, optional percentage default, manual-size state, pending restored state, drag state, and shell component-state binding.
The minimum revealed size is 50 logical pixels.
The component uses a `Gtk::Box`, gutter/toggle, `Gtk::Revealer`, and fixed-size pane wrapper rather than `Gtk::Paned`.

## Commands and transitions

### Responsive allocation

Each allocation selects compact at or below `compactMax`, regular at or below `regularMax`, and wide otherwise.
The component removes the old bucket class before applying `<prefix>-compact`, `<prefix>-regular`, or `<prefix>-wide`.
The default prefix is `ao-width`; an empty authored prefix falls back to that value.

### Raster targets

Image rendering chooses a target from current logical allocation and scale.
Fresh source loads render at high quality.
During repeated allocation churn for an already rendered source, GTK may show an immediate bilinear interim result and rerender the stable final size at high quality after the settle window.

### Collapsible split

Initial size comes from valid restored state, an authored positive position, or an allocation-derived percentage in that precedence order.
A restored size is clamped against the actual allocation before use.
Percentage sizing continues to follow allocations until a manual drag selects a concrete size.

The toggle changes revealer state and records current size/reveal state outside editor preview mode.
A capture-phase drag becomes a resize only after the three-logical-pixel threshold, preserving ordinary button clicks.
The delta adjusts the collapsible side according to orientation and side; the pane wrapper clips content beyond the remembered boundary.

## Failure and cancellation

Invalid child counts produce a visible layout-error widget.
Invalid/non-positive position falls through to percentage or the component default.
Unknown or stale component state is ignored without mutating the authored document.

Allocation callbacks and gesture controllers are detached when the component is destroyed.
An old component tree cannot write after the shell state generation advances.

## Persistence and versioning

Collapsible split size and reveal state are versioned component runtime state keyed by stable expanded node id.
Exact fields and guards belong to the [layout component-state reference](../../reference/shell/layout-state.md).
Panel-size promotion may move the current size into authored `position` and remove the promoted runtime entry.

Responsive bucket and image render target are derived and never persisted.

## Frontend observations

Main workspace structure should use expansion and percentage defaults.
Explicit width/height requests remain appropriate for local fixed slots such as icon buttons, transport controls, separators, cover targets, and popover controls.
Compressible custom children may request zero minimum along an axis so labels or entries do not force a panel beyond its allocation.

## Implementation map

- [`ResponsiveClassComponent.cpp`](../../../app/linux-gtk/layout/component/container/ResponsiveClassComponent.cpp) owns allocation buckets.
- [`CollapsibleSplitComponent.cpp`](../../../app/linux-gtk/layout/component/container/CollapsibleSplitComponent.cpp) owns pane construction, drag, reveal, and state.
- [`ImageWidget.cpp`](../../../app/linux-gtk/image/ImageWidget.cpp) owns scaled raster targets and stable rerendering.
- [`LayoutComponentState.cpp`](../../../app/uimodel/layout/component/LayoutComponentState.cpp) and [`LayoutStatePromoter.cpp`](../../../app/uimodel/layout/component/LayoutStatePromoter.cpp) own neutral state validation and promotion.

## Test map

- [`ResponsiveClassTest.cpp`](../../../test/unit/linux-gtk/layout/components/ResponsiveClassTest.cpp) protects allocation classification.
- [`CollapsibleSplitComponentTest.cpp`](../../../test/unit/linux-gtk/layout/components/CollapsibleSplitComponentTest.cpp) protects structure, size, restoration, toggling, persistence, and generation guards.
- [`ImageWidgetTest.cpp`](../../../test/unit/linux-gtk/image/ImageWidgetTest.cpp) and [`ImageRenderPolicyTest.cpp`](../../../test/unit/linux-gtk/image/ImageRenderPolicyTest.cpp) protect target sizing and rendering policy.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Shell layout lifecycle](layout-lifecycle.md)
- [Layout document reference](../../reference/shell/layout-document.md)
- [Layout component-state reference](../../reference/shell/layout-state.md)
- [Layout catalog reference](../../reference/shell/layout-catalog.md)
