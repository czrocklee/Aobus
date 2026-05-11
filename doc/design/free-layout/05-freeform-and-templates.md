# Phase 5: Freeform Layout and Templates

## Goal

Extend the structured layout system with reusable templates and optional freeform positioning. This phase should happen after YAML runtime loading, semantic components, and the structured editor are stable.

## Templates

Templates are named layout fragments that expand into normal layout nodes. They let users save a custom control cluster and reuse it in multiple layouts.

Example template storage:

```yaml
linuxGtkLayout:
  templates:
    compactPlayback:
      displayName: Compact Playback Controls
      root:
        type: box
        props:
          orientation: horizontal
          spacing: 4
        children:
          - type: playback.playPauseButton
          - type: playback.stopButton
          - type: playback.timeLabel
```

Template usage:

```yaml
type: template
props:
  templateId: compactPlayback
```

The runtime should expand templates before building GTK widgets. Recursive templates must be rejected.

Template expansion should track a visited-template stack:

```text
expand(templateId, visited)
  if templateId in visited -> error component / validation error
  push templateId
  expand children
  pop templateId
```

The validation error should include the full recursion chain so users can fix the template graph.

## Built-in Templates

Useful built-ins:

- `app.defaultLayout`
- `playback.defaultBar`
- `playback.compactControls`
- `library.defaultSidebar`
- `inspector.defaultPanel`
- `status.defaultBar`
- `tracks.defaultWorkspace`

Built-ins should behave like read-only templates that users can duplicate into editable user templates.

## Freeform Positioning

Freeform positioning should be additive, not the replacement for structured GTK layout. Add a dedicated container:

```yaml
type: absoluteCanvas
props:
  snapToGrid: true
  gridSize: 8
children:
  - type: playback.playPauseButton
    layout:
      x: 16
      y: 16
      width: 48
      height: 48
  - type: playback.seekSlider
    layout:
      x: 80
      y: 24
      width: 360
      height: 32
```

GTK4 does not provide `Gtk::Fixed` as a supported gtkmm layout primitive for this use case. Implement freeform positioning with a custom GTK4 layout approach, most likely a custom `Gtk::LayoutManager` or a custom widget/container that controls child measurement and allocation. Do not base the design on `Gtk::Fixed`.

## Freeform Layout Props

Common child layout props for `absoluteCanvas`:

```yaml
layout:
  x: 0
  y: 0
  width: 120
  height: 32
  minWidth: 40
  minHeight: 24
  zIndex: 0
  anchor: top-left
```

Potential future props:

```yaml
layout:
  left: 12
  right: 12
  top: 8
  bottom: null
  widthMode: fixed      # fixed | content | fill
  heightMode: content   # fixed | content | fill
```

Do not add anchors and constraints until basic x/y/width/height editing works.

`zIndex` should map to child ordering within the freeform container. The container should sort children by `zIndex` and stable insertion order before allocation/rendering. If GTK rendering order follows widget sibling order for the chosen implementation, reorder the internal child list to match the sorted order. Equal `zIndex` values preserve YAML child order.

RTL behavior must be explicit. Initial `x` coordinates can be physical left-to-right coordinates, but once anchors are added, `start`/`end` anchors should respect GTK text direction.

## Editor Support

Freeform editor features:

- Selection rectangle around the chosen child.
- Drag to move.
- Resize handles.
- Optional snap-to-grid.
- Keyboard nudging with arrow keys.
- Lock/unlock node.
- Z-order controls.

Start with numeric property editing for `x`, `y`, `width`, and `height`; add direct manipulation after persistence and rendering are reliable.

## Interaction with Semantic Components

Semantic components should not need to know whether they live in a `box`, `split`, or `absoluteCanvas`. They receive normal GTK size allocation and continue to use runtime typed subscriptions.

Components that need a minimum natural size should declare it through descriptors so the editor can warn users when they make a component too small.

## Versioning and Migration

Layout schema migrations should be explicit:

```text
version 1
  structured containers and semantic leaves

version 2
  templates

version 3
  absoluteCanvas and freeform layout props
```

Migration functions should operate on `LayoutDocument` before component instantiation:

```cpp
LayoutDocument migrateLayoutDocument(LayoutDocument doc, std::uint32_t targetVersion);
```

Unknown future versions should not be overwritten automatically. Load built-in default for the session and warn the user.

The in-memory document type should be able to represent all known schema versions. Version-specific fields such as `templates` should be optional/defaulted so v1 documents can migrate by adding empty template maps and later freeform defaults.

## Tests

- Template expansion preserves child order and props.
- Recursive template references are rejected.
- Missing template id renders an error component.
- `absoluteCanvas` persists x/y/width/height layout props.
- Freeform nodes remain valid through YAML round-trip.
- Version migration transforms older documents without losing known fields.

## Completion Criteria

- Users can save and reuse custom layout fragments.
- A freeform canvas can host semantic components at explicit positions.
- Freeform layout persists in YAML and reloads accurately.
- Structured layouts remain the default and continue to work unchanged.
