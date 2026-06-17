# GTK Responsive Layout

## Summary

GTK layout in Aobus is based on logical widget allocation, not physical monitor pixels. Display scale is used only where a component must render sharper raster content, such as cover art. Structural layout responds to the size GTK actually allocates to a widget tree.

## Responsibilities

YAML owns structural layout intent:

- container structure,
- expansion and alignment,
- default split percentages,
- explicit fixed slots for local controls.

CSS owns visual sizing:

- padding,
- borders,
- typography,
- hover and focus affordances,
- control hit targets.

C++ owns intrinsic and allocation-driven behavior:

- custom widget measurement,
- image render-target scaling,
- allocation breakpoints,
- user-resized split positions.

Display scale must not be used to compute panel widths, split defaults, or workspace geometry. Monitor geometry is reserved for real top-level display behavior such as a fullscreen overlay.

## Responsive Classes

`responsiveClass` is a decorator component that observes its own allocation and applies one class to its root widget:

- `ao-width-compact`
- `ao-width-regular`
- `ao-width-wide`

The default axis is width. `compactMax` and `regularMax` define the logical-pixel breakpoints. It is classified as `ComponentCategory::Decorator` rather than a container because it does not own layout structure; it only adds responsive CSS classes to its single child.

Use this component when a subtree needs compact/regular/wide styling based on the space it actually received. Do not branch on monitor resolution.

The modern header uses `responsiveClass` so the search control, title, and track count can adapt without a fixed search `widthRequest`.

## Size Requests

`widthRequest` and `heightRequest` are valid when the value describes a local fixed slot or control affordance, for example:

- icon buttons,
- transport controls,
- separators,
- cover-art targets,
- popover controls.

Avoid size requests for main workspace structure, side panes, or split defaults. Use percent-based split defaults and expansion policy instead.

`set_size_request(0, -1)` in custom widgets is allowed when it deliberately makes a child compressible so labels or entries do not force a panel wider than its allocation.

## Split Defaults

Default split positions should use percentages and be applied from allocation. A user-dragged position may be stored as a logical-pixel value because it represents an explicit user choice.
