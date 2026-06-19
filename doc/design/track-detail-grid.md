# Track Detail Grid Design

The Track Detail Grid is a unified component for displaying and editing track metadata and technical properties. It replaces individual, hardcoded metadata fields and audio property rows with a dynamic, data-driven layout.

## Data Model

The grid is driven by the `TrackDetailSnapshot`, which aggregates field values across the current selection.

### Built-in Fields

- Fields are sourced from `rt::trackFieldDefinitions()`.
- Synthetic fields (e.g., `DisplayTrackNumber`, `TechnicalSummary`, `Quality`) are excluded.
- Fields are categorized into `Metadata` and `Technical`.
- `Tag` fields are excluded from the grid as they are handled by the separate `track.tagEditor` component.

### Custom Metadata

- Custom metadata fields are aggregated across the selection.
- **Partial Presence**: A custom field may be present on some tracks but missing on others. This is indicated in the UI with a warning icon.
- **Multi-select Updates**: Updating a custom field value applies the change to all selected tracks, ensuring the property is present on all of them.
- **Deletion**: Deleting a custom property removes it from all selected tracks.

## UI Behavior

### Display Rules

- **No Selection**: The default detail pane keeps the same cover, field-grid,
  and tag-editor layout visible with an empty snapshot. The region is disabled,
  so the placeholder cannot submit edits. Selecting a track enables the same
  widget tree in place without moving the panel contents.
- **Mixed Values**: When selected tracks have different values for a field, the grid displays `<Multiple Values>`.
- **Unknown Values**: For technical fields, missing data is displayed as `Unknown`. For metadata fields, missing data is displayed as an empty string.

### Editing and Interaction

The Track Details panel uses an invisible interaction model. Editable controls appear only where the pointer or keyboard focus indicates intent.

- **Built-in Metadata**: Editable inline. They appear as plain text until hovered or focused, at which point an edit button (`document-edit-symbolic`) and subtle background highlight appear. Editing starts only from that button; clicking the value text does not activate the editor. Built-in metadata cannot be removed, only cleared (set to empty string).
- **Custom Metadata**: Editable inline and removable. Like built-in metadata, the value shows an edit button on hover/focus, and only that button activates editing. The row itself also shows a delete button (`user-trash-symbolic`) at the right edge when the row is hovered or receives focus.
- **Technical Fields**: Objective read-only properties (e.g., Sample Rate, Bitrate, File Path). They are styled slightly dimmer than editable fields and have no hover, focus, or cursor affordances.
- **Inline Editing**: Metadata and custom fields use a detail-field inline editor that displays as an ellipsizing label and switches to an entry while editing. Pressing `Enter` or clicking outside the active entry commits the change, while `Esc` cancels it. Outside-click handling does not depend on the clicked widget accepting keyboard focus. The UI refuses to save literal `<Multiple Values>`.
- **Single Edit Session**: Detail field editors are standard GTK compositions (`Gtk::Box`, `Gtk::Stack`, `Gtk::Label`, `Gtk::Entry`, and `Gtk::Button`). A shared coordinator allows only one active editor in the detail grid; activating another field commits the previous field before opening the next one.
- **Add Property**: An "Add Property" button allows users to define new custom metadata keys and values. Duplicate keys already present in the selection are rejected.
- **Delete Undo**: Deleting a custom metadata property shows a temporary (5-second) snackbar/undo bar at the bottom of the grid. Clicking "Undo" restores the property. Currently, this is only fully supported and presented when the deleted property had the same value across all selected tracks (not mixed).

### Import Boundary

Custom metadata represents properties created by the user inside Aobus, or explicitly provided through Aobus library import data. File tag readers do not promote unknown or vendor-specific MP4, ID3, or Vorbis fields into custom metadata; they only map fields that Aobus explicitly understands.

Custom keys are queryable regardless of their first character. ASCII alphanumeric/underscore keys
use the compact `%key` form, including numeric names such as `%123`; other keys use a quoted form
such as `%"Replay Gain"`. Metadata (`$`) and technical property (`@`) variables remain system
identifiers and must begin with an ASCII letter or underscore. See
[query-expression-language.md](query-expression-language.md) for the full syntax.

## Layout Configuration

The component is registered as `track.fieldGrid`. It supports a `categories` property to filter which field types to display.

Example:
```yaml
- type: track.fieldGrid
  props:
    categories: ["metadata", "technical"]
```

## Constrained Layout Behavior

The field grid uses a fixed one-field-per-row layout at every panel width.
Selecting a different track or resizing the split pane never changes the
structural row layout.

All rows use a 4-column `Gtk::Grid` so labels, values, warning icons, delete
buttons, section headers, and add-property rows have stable coordinates.

The grid is hosted in a scroll viewport with a fixed natural height. The
viewport shows eight field rows by default and uses a vertical scrollbar when
more fields are present. The field grid is vertically expandable, so it can use
extra space that the parent layout assigns to it, but adding or removing custom
metadata changes only the scrollable grid content. It does not change the
field section's requested height and therefore does not push cover art or
sibling detail widgets.

### Rows

Built-in rows are side-by-side: label occupies column 0, value occupies columns 1–3. Each row consumes 1 grid row.

Custom rows are single-row: label at column 0, editable value at column 1, partial-presence icon at column 2, delete button at column 3.

The add-property row mirrors the key/value split: the key entry occupies column
0, while the value entry and add action occupy columns 1–3.

Metadata, custom metadata, and technical fields are grouped under collapsible
section headers. Metadata and custom metadata are expanded by default; technical
audio properties are collapsed by default. Collapsing a section hides its field
rows but keeps the section header in the grid, so users can restore the section
without changing the surrounding layout.

A section header is a borderless full-width button that, at rest, renders only a
single full-bleed hairline rule spanning the row — the panel is dense, so no
section title text is shown. The disclosure chevron (`pan-down` when expanded,
`pan-end` when collapsed) is overlaid on the leading end of the rule rather than
placed before it, so it never displaces the line; it stays fully transparent at
rest and fades in only on hover or keyboard focus, at which point the whole
header strip also highlights. This keeps the divider clean and consistently
aligned while still surfacing the collapse affordance and current state on
demand.

Every grid cell is hosted by a clipped fixed-height slot. Row height is stable
across field values and font metrics; if a child widget internally asks for more
height, it is allocated enough height inside the clipped slot instead of
changing the row rhythm.

### Ordering

Content is always ordered: metadata header and rows, custom header with custom
rows and add-property row when tracks are selected, then the technical header and
technical rows.

## Text Stability

### Value Widgets

Metadata, custom metadata, and technical property values use a local detail-field
inline editor. In display mode it allocates an ellipsizing `Gtk::Label` to the
visible value width, so long values shorten inside the panel instead of raising
the grid's minimum width. Editable fields (metadata and custom) switch to a
`Gtk::Entry` only while editing; read-only technical values keep the display-only
contract.

When hovered or focused, editable values reveal an edit button. Hover and
keyboard-focus highlighting is confined to that button; the rest of the value
area remains visually unchanged. The button is the only pointer target that
enters edit mode; clicking the displayed value leaves it in display mode.
Read-only technical values do not show any hover affordance.

Text loaded from tags or custom metadata is normalized at the GTK display
boundary before it is passed to Pango. Invalid UTF-8 bytes are shown with Unicode
replacement characters in the detail panel, while the stored metadata bytes are
left untouched unless the user edits and saves the value.

All value widgets expose their full display text via tooltip.

Column expansion is managed through `hexpand` rules that are stable across track changes, preventing value length changes from resizing columns or shifting layout.
Key-column slots do not request horizontal expansion, and the add-property key
entry uses only a one-character natural-width hint so it fills the existing key
column without making that column compete with values during split-pane resize.

The key and value columns include zero-minimum width anchors that measure row
content from every section, including collapsed sections, and contribute only
natural widths to GTK's grid allocation. This keeps both columns from jumping
when metadata, custom properties, or technical audio properties are expanded or
collapsed, while preserving the panel's ability to report a zero horizontal
minimum and fit extremely narrow split-pane widths.

The field grid wrapper treats the detail panel allocation as the hard horizontal
limit. It does not report the grid's content-driven minimum or natural width to
its parent; instead, it computes vertical height from the panel width and clips
overflow. Long values can ellipsize inside the panel, but they cannot widen the
detail pane. Field labels keep their natural-width preference, but their GTK
minimum width is allowed to shrink so the grid can be allocated to the panel
width without pushing action controls outside the clipped area.
Key labels and action rows are hosted by zero-minimum clipped wrappers. The
labels, add-property button, warning icon, and delete action can keep their own
internal minimums, but those minimums do not raise the grid's minimum width or
force the grid to be allocated wider than the panel.

Sibling detail widgets must also avoid raising the pane minimum width. The tag
editor reports a zero horizontal minimum to its parent and clips its own internal
controls under extreme split resizing, so the field grid still receives the
actual panel width and can ellipsize values against the visible allocation.

Value widgets are configured with zero horizontal size request, hidden overflow,
and a one-character maximum natural width hint so value content cannot force a
wider grid column.

Custom metadata rows use a row widget inside the grid. The warning and delete
actions are allocated from the right edge of the panel first; the key and value
share the remaining width. Custom keys keep a readable width cap instead of a
one-character ellipsis, and the action controls remain anchored inside the panel.

## Cover Art Stability

The cover art widget in the detail pane uses a responsive square slot with a target size (default 250 in `default_layout.yaml`). For a given panel width, the slot's natural size is deterministic: it shrinks to the available width below the target and caps at the target size above it. Its minimum size remains compressible so animated or constrained parent allocations do not violate GTK measurement invariants. Track changes, image aspect ratio changes, and missing cover art do not change the slot footprint. The `ImageWidget` renders inside that square and clips overflow, preventing cover art from consuming space needed by fields and tags.

## Track Row Display State

Track list rows cache display strings on the `TrackRowObject` so recycled GTK cells can bind without
reformatting computed fields on every scroll step. Text-backed fields are stored directly; computed
fields are memoized until a contributing row value changes.

Now-playing row styling is model-driven rather than per-row property-driven. `TrackListModel`
records the current playing track id and emits a dedicated playing-changed signal for visible cells
to restyle in place. Newly bound or recycled rows are stamped from the same model state when GTK asks
for the item.
