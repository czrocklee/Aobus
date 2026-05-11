# Phase 4: Layout Editor

## Goal

Add an in-application layout editor that lets users modify the YAML-backed layout without hand-editing YAML. The editor should support structured layout editing first and freeform positioning later.

## Editing Modes

The application should have two layout modes:

```text
Run Mode
  normal application behavior

Edit Mode
  same layout rendered with selection outlines, insert targets, and property panels
```

Edit mode should be reversible. Users can cancel changes and return to the previously loaded layout.

The editor should work on a deep copy of the active `LayoutDocument`. The running document is replaced only when the user explicitly applies or saves. Cancel discards the edited copy and rebuilds the host from the original document if preview changes were applied.

## Editor Shell

```diagram
╭────────────────────────────────────────────────────────────╮
│ Layout Editor                                              │
├───────────────┬───────────────────────────┬────────────────┤
│ Palette       │ Live Preview              │ Properties     │
│               │                           │                │
│ Containers    │ selected node outline     │ selected node  │
│ Playback      │ insertion handles         │ props          │
│ Library       │ split drag handles        │ layout props   │
│ Tracks        │                           │                │
│ Inspector     │                           │                │
└───────────────┴───────────────────────────┴────────────────┘
```

The editor can be a modal window, separate top-level window, or an overlay around `LayoutHost`. Start with the simplest implementation: a dialog containing a tree outline and property editor, with live preview optional.

Ownership split:

- `LayoutHost` owns the edit-mode overlay layer around the live layout, including selection rectangles, insertion targets, and resize handles.
- `LayoutEditor` owns the palette, tree outline, property editors, validation messages, and save/apply/cancel flow.
- `LayoutEditor` drives changes by editing a document copy and asking `LayoutHost` to preview or apply it.

This keeps visual decorations close to the rendered widget tree while keeping editing workflow and document mutation out of `LayoutHost`.

## Component Descriptors

Each registered component should expose metadata for the editor:

```cpp
struct PropertyDescriptor final
{
  std::string name;
  PropertyKind kind;
  std::string label;
  LayoutValue defaultValue;
  std::vector<std::string> enumValues;
};

struct ComponentDescriptor final
{
  std::string type;
  std::string displayName;
  std::string category;
  bool container = false;
  std::vector<PropertyDescriptor> props;
  std::vector<PropertyDescriptor> layoutProps;
  std::size_t minChildren = 0;
  std::optional<std::size_t> maxChildren;
};
```

Descriptors are editor metadata. Runtime behavior must not depend on descriptors being exhaustive.

Initial `PropertyKind` values should include:

- `Bool` -> checkbox
- `Int` -> spin button
- `Double` -> spin button with decimal precision
- `String` -> entry
- `Enum` -> dropdown
- `StringList` -> tag/chip editor or newline-separated entry in the first version
- `CssClassList` -> specialized string-list editor with class-name validation
- `Size` -> integer spin button with optional unset state

## Initial Editable Operations

Phase 4 should support these operations first:

- Select a node in a tree outline.
- Rename node `id`.
- Change component props through typed editors.
- Change common layout props: margin, hexpand, vexpand, halign, valign, min size.
- Add component as child of a selected container.
- Remove selected node.
- Move selected node up/down among siblings.
- Wrap selected node in `box`, `split`, `tabs`, or `scroll`.
- Save layout to YAML.
- Reset to built-in default layout.

Drag-and-drop visual editing can wait until these tree-based operations work reliably.

## Property Editing Examples

### Playback Button

```yaml
type: playback.playPauseButton
props:
  showLabel: false
  size: large
layout:
  margin: 2
```

Editor fields:

- Show label: checkbox
- Size: enum `small | normal | large`
- Margin: integer

### Split Container

```yaml
type: split
props:
  orientation: horizontal
  position: 330
  shrinkStart: false
  shrinkEnd: false
```

Editor fields:

- Orientation: enum `horizontal | vertical`
- Position: integer
- Shrink start/end: checkbox

## Live Preview Strategy

The safest strategy is full subtree rebuild after every edit:

```text
modify LayoutDocument
       │
       ▼
validate subtree
       │
       ▼
LayoutRuntime rebuilds root or edited subtree
```

Live mutation methods such as `updateProps()` are optional and should only be added for performance after the editor behavior is correct.

Full rebuild can flicker and can drop transient widget state. Mitigations:

- Rebuild only the smallest valid subtree when possible.
- Temporarily hide the preview container during large rebuilds.
- Preserve selected high-value state explicitly, such as selected editor node, active tab, split position, and scroll position.
- Avoid rebuilding on every keystroke for text fields; debounce or apply on commit.

## Validation Rules

The editor should prevent or warn about invalid layouts:

- Root must exist.
- Every node must have a known `type` unless the user is intentionally preserving an unknown component from a future version.
- `split` must have exactly two children.
- `scroll` must have exactly one child.
- `tabs` must have at least one child.
- Components marked as leaf must not have children.
- Required component props must be present or defaultable.

Unknown future-version nodes should be visible in the tree and preserved if possible, but they cannot be rendered except as error placeholders.

To preserve unknown nodes and future-version fields, `LayoutNode` should either keep an opaque YAML field bucket or `LayoutValue` should support nested maps/sequences rich enough to round-trip unrecognized data. This is required before the editor claims future-version preservation.

## YAML Save Semantics

When saving, preserve human-readable YAML:

```yaml
version: 1
root:
  id: root
  type: box
  props:
    orientation: vertical
  children:
    - type: playback.playPauseButton
    - type: tracks.table
      layout:
        vexpand: true
```

Avoid dumping implementation-only defaults unless the user explicitly changed them. This keeps layout files readable and editable outside the application.

## Tests

- Descriptor validation catches invalid child counts.
- Editor operations produce valid YAML documents.
- Reset to default replaces the active document but does not delete user-saved layouts unless requested.
- Unknown nodes are preserved through load/save if not edited.
- Save/cancel paths are covered.

## Completion Criteria

- Users can rearrange the default layout using a tree/property editor.
- Users can build a custom playback control row from semantic playback components.
- Saved YAML reloads on next startup.
- Invalid editor operations are blocked or produce visible validation errors.
