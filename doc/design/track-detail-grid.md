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
- **Multi-select Updates**: Updating a custom metadata value applies the change to all selected tracks, ensuring the key is present on all of them.
- **Deletion**: Deleting custom metadata removes it from all selected tracks.

## UI Behavior

### Display Rules

- **No Selection**: The default detail pane keeps the same cover, field-grid,
  and tag-editor layout visible with an empty snapshot. The region is disabled,
  so the placeholder cannot submit edits. Selecting a track enables the same
  widget tree in place without moving the panel contents.
- **Mixed Values**: When selected tracks have different values for a field, the grid displays `<Multiple Values>`.
- **Unknown Values**: For technical fields, missing data is displayed as `Unknown`. For metadata fields, missing data is displayed as an empty string.
- **Empty Metadata**: Metadata rows whose current display value is empty are
  hidden by default. Mixed values and any non-empty formatted value remain
  visible. The Metadata section includes a fixed-height action row with a
  "Show empty fields" control that reveals empty rows for editing and toggles
  back to "Hide empty fields" while they are visible. Custom metadata rows use
  the same empty-value visibility rule because they are part of the Metadata
  section.

### Editing and Interaction

The Track Details panel uses an invisible interaction model. Editable controls appear only where the pointer or keyboard focus indicates intent.

- **Built-in Metadata**: Editable inline. They appear as plain text until hovered or focused, at which point an edit button (`document-edit-symbolic`) and subtle background highlight appear. Editing starts only from that button; clicking the value text does not activate the editor. Built-in metadata cannot be removed, only cleared (set to empty string).
- **Custom Metadata**: Editable inline and removable. Like built-in metadata, the value shows an edit button on hover/focus, and only that button activates editing. The row itself also shows a delete button (`user-trash-symbolic`) at the right edge when the row is hovered or receives focus.
- **Technical Fields**: Objective read-only properties (e.g., Sample Rate, Bitrate, File Path). They are styled slightly dimmer than editable fields and have no hover, focus, or cursor affordances.
- **Inline Editing**: Metadata and custom fields use a detail-field inline editor that displays as an ellipsizing label and switches to an entry while editing. Pressing `Enter` or clicking outside the active entry commits the change, while `Esc` cancels it. Outside-click handling does not depend on the clicked widget accepting keyboard focus. The UI refuses to save literal `<Multiple Values>`.
- **Single Edit Session**: Detail field editors are standard GTK compositions (`Gtk::Box`, `Gtk::Stack`, `Gtk::Label`, `Gtk::Entry`, and `Gtk::Button`). A shared coordinator allows only one active editor in the detail grid; activating another field commits the previous field before opening the next one.
- **Add Custom Metadata**: An add button in the Metadata action row opens a popover
  where users define new custom metadata keys and values. Duplicate keys
  already present in the selection are rejected.
- **Delete Undo**: Deleting custom metadata shows a temporary
  (5-second) undo bar through `track.detailUndoBar`. Clicking "Undo" restores
  the key and value. Currently, this is only fully supported and presented when
  the deleted key had the same value across all selected tracks (not mixed).
  Pending undo is cleared when the detail selection changes, because the undo
  bar is scoped to the current detail pane rather than a global notification
  queue. Pending undo is also cleared when the same custom metadata key is written
  again for any overlapping track in the same detail selection.

### Import Boundary

Custom metadata represents key/value metadata created by the user inside Aobus, or explicitly provided through Aobus library import data. File tag readers do not promote unknown or vendor-specific MP4, ID3, or Vorbis fields into custom metadata; they only map fields that Aobus explicitly understands.

Custom keys are queryable regardless of their first character. ASCII alphanumeric/underscore keys
use the compact `%key` form, including numeric names such as `%123`; other keys use a quoted form
such as `%"Replay Gain"`. Metadata (`$`) and technical property (`@`) variables remain system
identifiers and must begin with an ASCII letter or underscore. See
[query-expression-language.md](query-expression-language.md) for the full syntax.

## Layout Configuration

The component is registered as `track.fieldGrid`. It supports a `categories`
property to filter metadata and technical field rows. Custom metadata is part
of the Metadata section, so a technical-only grid (`categories: ["technical"]`)
does not render the custom metadata rows or add button.

Custom metadata deletion undo is exposed by the sibling
`track.detailUndoBar` component. The default detail pane wraps
`track.fieldGrid` in a layout-owned `scroll` component and places
`track.detailUndoBar` outside that scroll, so the undo bar stays visible while
the field list scrolls.

Example:
```yaml
- type: scroll
  props:
    hscrollPolicy: never
    vscrollPolicy: automatic
  layout:
    vexpand: true
  children:
    - type: track.fieldGrid
      props:
        categories: ["metadata", "technical"]
- type: track.detailUndoBar
```

## Constrained Layout Behavior

The field grid uses a fixed one-field-per-row layout at every panel width.
Selecting a different track or resizing the split pane never changes the
structural row layout.

All field rows use a 4-column `Gtk::Grid` so labels, values, section headers,
and metadata action rows have stable coordinates.

The grid itself does not create a scroll boundary. The default detail layout
owns scrolling by wrapping `track.fieldGrid` in a `scroll` component. Adding or
removing custom metadata changes the scrollable content while sibling detail
widgets, including the undo bar and tag editor, stay outside the field-list
scroll area.

### Rows

Built-in rows are side-by-side: label occupies column 0, value occupies columns 1–3. Each row consumes 1 grid row.

Custom rows are single-row: label at column 0 and a clipped value cell spanning
columns 1-3. The value cell contains the editable value, partial-presence icon,
and delete button.

The Metadata action row spans the full grid width. It places the
Show/Hide empty fields control on the left and the custom metadata add button on
the right. The add button opens a popover for the key/value inputs instead of
keeping a permanent form row in the grid.

Metadata and technical fields are grouped under top-level collapsible section
headers. Custom metadata is part of the Metadata section rather than a separate
top-level section. Metadata is expanded by default; technical audio properties
are collapsed by default. Collapsing Metadata hides built-in metadata rows, the
Metadata action row, and custom metadata rows. Collapsing a section hides its
field rows but keeps the section header in the grid, so users can restore the
section without changing the surrounding layout.

A section header is a borderless full-width button with a full-bleed hairline
rule, an overlaid text label, and a disclosure chevron (`pan-down` when
expanded, `pan-end` when collapsed). The chevron is overlaid on the leading end
of the rule rather than placed before it, so it never displaces the line; it
stays fully transparent at rest and fades in only on hover or keyboard focus, at
which point the whole header strip also highlights. Expanded headers show their
section title. Collapsed Metadata summarizes the selected title and artist when
available, while collapsed Audio Properties summarizes codec, sample rate, and
bit depth when available.

Section header and empty-field control styling is theme-aware. Classic keeps
the controls dense, square, and high-contrast like a data panel; Modern uses the
same widget structure with more breathing room, uppercase micro-labels, softer
rules, and accent-colored link affordances.

Every grid cell is hosted by a clipped fixed-height slot. Row height is stable
across field values and font metrics; if a child widget internally asks for more
height, it is allocated enough height inside the clipped slot instead of
changing the row rhythm.

### Ordering

Content is always ordered: Metadata header, built-in metadata rows, Metadata
action row, custom metadata rows when tracks are selected, then the Audio
Properties header and technical rows.

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
Key-column slots do not request horizontal expansion, and the add metadata key
entry uses only a one-character natural-width hint so it fills the existing key
column without making that column compete with values during split-pane resize.

The key and value columns include zero-minimum width anchors that measure row
content from every section, including collapsed sections, and contribute only
natural widths to GTK's grid allocation. This keeps both columns from jumping
when metadata, custom metadata, or technical audio properties are expanded or
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
labels, custom metadata add button, warning icon, and delete action can keep their own
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
