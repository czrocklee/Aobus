# Layout Component State

GTK layout component state is keyed by stable layout node ids, not by tree
position. Moving a stateful component in the layout tree must not change its
future runtime-state key.

Runtime state files are stored under `$XDG_STATE_HOME/aobus/layout-state` (or the
platform equivalent), not under the config directory.

Stateful layout component types currently include:

- `split`
- `collapsibleSplit`

These type names are declared as constants in `app/include/ao/uimodel/layout/component/StatefulLayoutComponentType.h`
and used by runtime-state logic, baseline hashing, and promotion so the set of
stateful types is maintained in one place.

Built-in layout presets must assign ids to every stateful component node,
including stateful nodes declared inside reusable templates. Anonymous stateful
nodes remain valid layout nodes, but they are non-persistent and produce a
diagnostic in the shared layout model.

The layout editor generates unique ids for newly added nodes and wrapper
containers. Save validation rejects duplicate ids among stateful nodes after
template expansion, because those duplicates would make future runtime
component state ambiguous.

`split` components persist the user's divider position as `positionPercent` in
the active preset's runtime state file. A matching component id, type, and
baseline hash restores that percent on the next allocation; mismatches fall back
to the layout-authored `position` or `initialPositionPercent` defaults. Ordinary
splitter movement updates runtime state only and does not rewrite the layout
document.

`collapsibleSplit` components persist the collapsible pane's logical GTK pixel
`size` plus its `revealed` state in the active preset's runtime state file. A
matching component id, type, and baseline hash restores `revealed` immediately
and restores `size` after the component receives a valid allocation, clamping
wide-window sizes so the non-collapsible side remains usable on narrower
windows. Baseline mismatches fall back to the layout-authored `position`,
`initialPositionPercent`, and `revealed` defaults. Dragging the resize gutter
or toggling the pane updates runtime state only and does not rewrite the layout
document; editor preview interactions do not write runtime state.

The View menu exposes explicit commands for state ownership boundaries:

- `Reset Runtime Layout State` deletes the active preset's runtime state file
  and rebuilds the current layout from layout defaults. User-authored layout
  YAML is preserved.
- `Save Current Panel Sizes as Layout Defaults` promotes matching runtime panel
  sizes into the active preset layout YAML. `split.positionPercent` becomes the
  layout `initialPositionPercent`, while `collapsibleSplit.size` becomes the
  layout `position`. Promoted size entries are removed from runtime state; any
  non-promoted runtime values, such as `collapsibleSplit.revealed`, are kept
  with a refreshed baseline hash.
