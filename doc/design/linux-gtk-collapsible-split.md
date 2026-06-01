# Linux GTK Collapsible Split

`collapsibleSplit` is the layout primitive used for IDE-style side panes: the
collapsible side can be resized with a narrow border grip and can be fully hidden
or revealed by clicking a separate button.

## Structure

The component is implemented with `Gtk::Box`, not `Gtk::Paned`. `Gtk::Paned`
keeps splitter allocation semantics even when one child is hidden, which makes a
true zero-width collapsed pane difficult to guarantee.

For an end-collapsing horizontal split, the widget tree is:

```text
Gtk::Box
|-- workspace child
|-- Gtk::Box resizeGrip
|-- Gtk::Button toggleButton
`-- Gtk::Revealer
    `-- Gtk::Box paneSizer
        `-- detail child
```

The non-collapsible child expands along the split orientation. The collapsible
child is wrapped in `paneSizer`, and only that wrapper receives explicit size
requests. The resize grip and toggle button are distinct widgets so drag and
click handling do not share the same GTK event target.

## Behavior

- `position` is the remembered size of the collapsible pane, not a `Gtk::Paned`
  divider coordinate.
- Invalid or non-positive `position` values fall back to the default pane size.
- Clicking the toggle button toggles `Gtk::Revealer::set_reveal_child()`.
- Dragging the resize grip updates `paneSizer.set_size_request()` along the split
  orientation.
- The drag controller is attached to the outer container, not the resize grip.
  This keeps drag offsets in a stable coordinate space while resizing moves the
  grip itself.
- During drag, the native surface cursor is temporarily set to the resize cursor
  so the cursor stays stable even if the pointer leaves the narrow resize grip.

`resizeStart`, `resizeEnd`, `shrinkStart`, and `shrinkEnd` are intentionally not
part of this component. Those properties belong to the regular `split`
component, which is backed by `Gtk::Paned`.
