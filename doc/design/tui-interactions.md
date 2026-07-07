# TUI Interactions

The TUI keeps the track workspace as the primary surface. Overlays are modal:
while an overlay is open, workspace-only shortcuts do not act on the track table
underneath it.

## Visual Structure

- Generic TUI chrome inherits the terminal theme. Ordinary text uses the default
  foreground/background, secondary text is dimmed, and semantic accents use ANSI
  roles rather than hard-coded application backgrounds.
- Panel titles, command-palette categories, table headers, active output badges,
  and shortcut keys use the accent role to make the interface less monochrome
  without introducing broad colored backgrounds.
- Help, detail, list, view, command, quality, and output panels use titled frame
  chrome. Titles are integrated into the frame instead of consuming a separate
  title row.
- Popover panel bodies keep one terminal cell of horizontal padding inside the
  frame so text and selected rows do not touch the border. The track workspace
  frame remains unpadded to preserve table density.
- The track workspace uses a clean top frame edge; its active list and active
  view live on the lower-left frame edge as `list <name>` and `view <name>`,
  separated by an uncolored frame line. The selection/count summary sits on the
  lower-right frame edge. Detail and help side panes stay beside that workspace
  instead of replacing it.
- Selected rows and hovered controls use one centralized interactive surface:
  ANSI yellow background, ANSI black foreground, and bold. Row markers such as
  `>` are visible affordances, not the only selection signal.

## Input Boundaries

- `App` owns one `TuiHitRegions` instance for reflected boxes and row hit
  regions. Render code writes the frame's boxes into that owner; the event
  controller reads it for mouse dispatch and hover hit testing.
- `ListNavigation` owns the shared keyboard mapping for list-like surfaces:
  `Up`/`Down`, `PageUp`/`PageDown`, and `Home`/`End` all pass through that
  helper before a surface-specific move callback applies the delta.
- `SelectableList` owns shared list rendering mechanics for list-like panels:
  selected-row styling, optional row-box reflection, focus positioning, frame
  chrome, scroll indicator, empty text, and fixed list height. Callers still own
  row labels, active markers, panel titles, footers, and input/navigation.

## Playback Dock and Seek Rail

- The playback dock is a single row. It starts with a Soul-style button that
  combines transport control and quality color, followed by output, title/artist,
  elapsed time, seek rail, duration, and `Vol N%`.
- The seek rail expands with terminal width within bounded limits so wide
  terminals spend extra space on the timeline instead of empty title padding.
- The Soul button is width-stable at three terminal cells. Playing uses those
  cells as a tiny braille canvas where only a partial abstract arc rotates and
  breathes between narrow, balanced, and wide spans. The brand color and timing
  recipe come from `uimodel/playback/soul`: the quality aura stays dominant, the
  UI cyan-core to quality-aura gradient is projected onto the visible arc with
  its 0.382 core stop, hue drifts by ±10°, arc breadth follows the shared
  stroke-width breathing phase, and overall luminance follows the opacity period
  with the 0.618 floor. Paused freezes into a static arc, idle and stopping keep
  a dim dormant-cyan core, transient opening/buffering/seeking states run the
  same arc at the transient pulse cadence, and errors use `!!!`. Clicking it
  toggles play/pause; hovering
  it shows the quality panel without opening a modal overlay. The `a` shortcut
  still opens the quality panel explicitly.
- The seek rail reflects only the rail/thumb segment for hit testing. Elapsed and
  duration labels are display-only.
- Clicking or dragging the seek rail previews while the pointer is active and
  commits a final seek on release. Release outside an active drag clamps to the
  rail range. Duration-zero rails ignore pointer input.
- Command mode and overlays are modal for the seek rail. A rail click does not
  start seeking while either is active, and an active rail drag is canceled if
  command mode or an overlay becomes active.
- Short terminals collapse the playback dock to one row before sacrificing more
  track-table height.

## Status and Notifications

- The lower-left status slot shows the shared activity-status compact state from
  the runtime notification feed. TUI feedback such as `Ready` and panel open/close
  messages is posted into that feed and auto-dismisses through the shared model.
- `n`, `/notifications`, and clicking the status slot open the notification
  center when there are details. Clearable rows can be hidden from the activity
  presentation without dismissing the underlying runtime notification feed.
- Transient notification compacts use the shared auto-dismiss timeout from the
  activity model. Warning and error notification priority remains in the shared
  model rather than the TUI renderer.

## Command Palette

- Command completion renders as a command palette with a selected-row marker,
  optional TUI-local command category, command text, and right-aligned shortcut
  or detail text.
- The command palette uses a centered ratio-sized frame rather than content-sized
  columns: about 40% of terminal width and 35% of terminal height, with practical
  min/max clamps for narrow and tall terminals.
- Category and shortcut metadata belong to the TUI command specs. Runtime/list/
  query completions that do not have TUI metadata continue to render their core
  completion detail text.
- Command input uses a titled frame when the terminal has enough rows and falls
  back to one line on short terminals.

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

- `l`, `v`, `n`, `d`, `a`, and `o` toggle the list, view, notification, detail,
  quality, and output overlays.
- Clicking the library name, view indicator, status slot, quality indicator, or
  output indicator opens the matching overlay at that control.
- `Enter` activates the selected row in list, view, and output overlays.
- `Esc` closes the current overlay.
