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

- **Mixed Values**: When selected tracks have different values for a field, the grid displays `<Multiple Values>`.
- **Unknown Values**: For technical fields, missing data is displayed as `Unknown`. For metadata fields, missing data is displayed as an empty string.

### Editing

- **Edit Lock**: Editable rows are guarded by the global `track.editLock`. When locked, fields are read-only.
- **Inline Editing**: Metadata and custom fields use `Gtk::EditableLabel` for immediate, inline editing.
- **Technical Fields**: Technical fields (e.g., Sample Rate, Bitrate, File Path) are always read-only.
- **Add Property**: An "Add Property" button allows users to define new custom metadata keys and values. Duplicate keys already present in the selection are rejected.

## Layout Configuration

The component is registered as `track.fieldGrid`. It supports a `categories` property to filter which field types to display.

Example:
```yaml
- type: track.fieldGrid
  props:
    categories: ["metadata", "technical"]
```

## Responsive Behavior

The field grid adapts its layout automatically to the allocated panel width. The layout mode is driven only by panel width, not by track content. Selecting a different track never changes the structural layout mode.

### Breakpoints

| Mode       | Width            |
|------------|------------------|
| Standard   | `< 550px`        |
| Wide       | `>= 550px`       |

All modes use a 4-column `Gtk::Grid` so custom rows, warning icons, delete buttons, separator rows, and add-property rows have stable coordinates.

### Standard

Built-in rows are side-by-side: label occupies column 0, value occupies columns 1–3. Each row consumes 1 grid row.

Custom rows are single-row: label at column 0, editable value at column 1, partial-presence icon at column 2, delete button at column 3.

### Wide

Built-in metadata and technical rows are packed two items per grid row:
- Item A: label at column 0, value at column 1
- Item B: label at column 2, value at column 3

Row count is `(count + 1) / 2`, so an odd count leaves the second slot empty on the last row.

Custom rows remain single-item rows (same as Standard) because they need warning/delete controls.

The add-property row and separator row span all 4 columns (same as Standard).

### Ordering

Content is always ordered: metadata rows, custom rows, add-property row, separator (when technical rows are present and metadata/custom rows exist), technical rows.

## Text Stability

### Value Widgets

Metadata, custom metadata, and technical property values use `Gtk::EditableLabel`
as their single value widget. Editable fields become editable when the detail
scope is unlocked; read-only technical values keep the same widget contract but
remain non-editable. This keeps display and editing width behavior identical and
avoids measuring a different widget when editing starts or ends.

When editing is unlocked, editable values receive an active editor affordance
with a subtle background and border. Locked values and read-only technical
values do not show that affordance.

Text loaded from tags or custom metadata is normalized at the GTK display
boundary before it is passed to Pango. Invalid UTF-8 bytes are shown with Unicode
replacement characters in the detail panel, while the stored metadata bytes are
left untouched unless the user edits and saves the value.

All value widgets expose their full display text via tooltip.

Column expansion is managed through `hexpand` rules that are stable across track changes, preventing value length changes from resizing columns or shifting layout.

The field grid wrapper treats the detail panel allocation as the hard horizontal
limit. It does not report the grid's content-driven minimum or natural width to
its parent; instead, it computes vertical height from the panel width and clips
overflow. Long values can ellipsize inside the panel, but they cannot widen the
detail pane. Field labels keep their natural-width preference, but their GTK
minimum width is allowed to shrink so the grid can be allocated to the panel
width without pushing action controls outside the clipped area.

Value widgets are configured with zero horizontal size request, hidden overflow,
and a one-character maximum natural width hint so value content cannot force a
wider grid column.

Custom metadata rows use a row widget inside the grid. The warning and delete
actions are allocated from the right edge of the panel first; the key and value
share the remaining width. Custom keys keep a readable width cap instead of a
one-character ellipsis, and the action controls remain anchored inside the panel.

## Cover Art Stability

The cover art widget in the detail pane uses a responsive square slot with a target size (default 250 in `default_layout.yaml`). For a given panel width, the slot's natural size is deterministic: it shrinks to the available width below the target and caps at the target size above it. Its minimum size remains compressible so animated or constrained parent allocations do not violate GTK measurement invariants. Track changes, image aspect ratio changes, and missing cover art do not change the slot footprint. The `ImageWidget` renders inside that square and clips overflow, preventing cover art from consuming space needed by fields and tags.
