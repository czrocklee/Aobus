# TUI Interactions

The TUI keeps the track workspace as the primary surface. Overlays are modal:
while an overlay is open, workspace-only shortcuts do not act on the track table
underneath it.

## Workspace

- `Up` / `Down` move the selected track.
- `PageUp` / `PageDown` move the selected track by a page step.
- `Home` / `End` move to the first or last track.
- `{` / `}` jump to the previous or next projection group when the active
  presentation exposes group sections.
- Mouse wheel events over the track table move the selected track.
- Dragging the table scrollbar moves selection by visual row. Group headers
  count as visual rows, but selection always lands on a track row.
- Dragging a column edge in the table header resizes that column for the current
  TUI session.
- Clicking a group section header jumps to the first track in that section.

## Overlays

- `l`, `v`, `d`, `a`, and `o` toggle the list, view, detail, quality, and output
  overlays.
- Clicking the library name, view indicator, quality indicator, or output
  indicator opens the matching overlay at that control.
- `Enter` activates the selected row in list, view, and output overlays.
- `Esc` closes the current overlay.
