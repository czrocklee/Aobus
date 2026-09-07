---
id: user.use-tui
type: user-guide
status: current
domain: tui
summary: Opens an indexed library and controls selection, playback, filters, and overlays in the terminal frontend.
---
# Use the terminal frontend

## Outcome

You can browse an existing Aobus library, play tracks, filter the current view, and inspect detail, quality, output, presentation, and notification panels from a terminal.

## Before you start

The TUI opens an Aobus database; it does not create one.
Initialize the root with the GTK application or CLI first, then scan from any shell.

## Steps

1. Start the TUI for the indexed root:

   ```bash
   aobus-tui --library /music
   ```

2. Move the cursor with Up/Down, PageUp/PageDown, Home, and End.
3. Press Enter to play the focused track.
   Use Space for play/pause, `s` to stop, `[` and `]` to seek by five seconds, and `-`/`+` to change volume by five percentage points.
4. Press `/` to open Quick Filter in the bottom status bar, then type to filter the current view live; suggestions open directly above the input.
   Up/Down selects a suggestion, Tab accepts it while you keep editing, and Enter accepts it and closes Quick Filter.
   Escape keeps the text you typed instead of the selected suggestion.
   To clear the current filter, press `c` in the workspace or press `/` followed immediately by Enter; `/` followed immediately by Escape preserves it.
5. Press `:` to open the Command Palette, then enter a command such as:

   ```text
   :filter $composer == "Bach"
   :view classical-works
   ```

   Tab accepts a highlighted command completion; Enter runs only a complete known command.
   `:scan` and `:rescan` start an eager library scan; `:scan cancel` requests cooperative cancellation.
   A scan already in progress, or a cancellation still settling, posts a short notice instead of starting a second flight.
   Progress uses the existing status line; the finished scan uses the same outcome sentence as the other shells.
   `m` marks or unmarks the focused track without moving the cursor. `v` starts a vim-style visual selection at the cursor: move with `j`/`k` or the arrow keys and the range grows as you go, `v` again keeps it, Escape throws it away and restores the marks you had before. While the selection runs the status line shows `VISUAL` in front of the counts, and Escape cancels it even with the detail pane open, which stays open. `Shift+A` marks every track in the current view, including rows off screen. `u` clears marks so the effective selection falls back to the focused track.
   A marked row reverses its own foreground and background, so it follows your terminal color scheme instead of a fixed color; the cursor row reverses its yellow highlight the same way when it is marked.
   Playback, Detail, and cover art still follow the cursor. Enter plays the focused track even when other rows are marked. When marks exist, the status line shows how many tracks are marked.
   Opening a different List or applying a new filter clears marks and returns the cursor to the top; confirming the current List or reloading keeps both the marked ids that are still in the view and the cursor.
6. Press `e`, `:edit`, or `:properties` to open the Track Properties editor over the current selection: every marked track, or the focused track when nothing is marked.
   The editor appears as a centered modal dialog floating over the dimmed workspace. Its target list is frozen when it opens, so editing a different set of tracks means closing it and opening a new one.
   Press Tab or Shift+Tab to switch between the editor's pages: `Metadata`, `Tags`, `Properties` (read-only audio/technical details), and (for multi-track selections) `Tracks` (to review captured titles and paths).
   On the `Metadata` page, editing is direct without checkboxes: Up and Down move between rows, and typing immediately edits the focused field.
   An edited field displays a passive changed marker (`*`), while an active indicator (`>`) marks the focused row.
   Only changed fields are written to every target; untouched fields preserve each track's existing value.
   A field shared across all targets shows its value, while differing fields display a multiple-values placeholder; typing there replaces that field for all targets.
   To clear a field across all targets, press Ctrl+U; pressing Ctrl+G restores the field to its baseline value or mixed preservation state.
   For supported fields (such as Artist, Album, Genre, Composer), typing suggests matching library values in a popup; press Ctrl+N to open completion explicitly. Use Up/Down to navigate suggestions, Enter to accept, and Escape to dismiss the popup.
   On the `Tags` page, the box in front of each tag shows what the selected tracks would carry after you apply: `[x]` all of them, `[ ]` none of them, and `[~]` only some of them.
   A `[~]` row also shows the fraction, such as `1/2`, because the box alone cannot say how many. Below your own tags come library tags none of the selection carries yet, which is what their empty boxes say.
   Just type: the query field above the list is always live, filtering as you go, and Up/Down move the selection. The list shows the 50 most-used suggestions, so typing is also how you reach a tag used too rarely to be listed.
   Press Enter to act on the selected tag: the box moves to where the tag is going and turns colour, with `Add` or `Remove` named beside it. A tag none of the targets carries is marked for adding, a tag all of them carry is marked for removal, and a tag only some of them carry cycles through both before returning to unchanged. Ctrl+G restores the selected tag directly.
   If what you typed names no existing tag, the last row offers to create it; press Enter there to add it to all targets. Escape clears the query before it closes the editor.
   Press Ctrl+S to apply the unified changes across metadata and tags; the footer displays pending field changes, clears, tag additions, and tag removals before you save.
   Press Escape to close or Ctrl+R to re-read the tracks and start fresh; if changes were made, both prompt for confirmation (Enter confirms, Escape cancels).
   Any library change while the editor is open marks the draft stale and disables Ctrl+S; your draft is preserved so you can press Ctrl+R to reload or Escape to leave.
   Applying closes the editor and reports the number of deduplicated changed tracks. Tracks that already matched the submitted values are not counted, and a save requiring no change reports "No changes were needed".
7. Toggle panels with `l` for lists, `d` for detail, `a` for the quality pipeline, `o` for output devices, `p` for presentations, and `n` for notifications.
8. Press `d` and keep browsing: the detail pane stays open beside the track table and follows the cursor, so arrows, pages, wheel, scrollbar, group jumps, playback, and filtering all keep working while you read it.
   Press `d` again or Escape to close it.
9. With mouse tracking enabled, drag a track-header column edge to preview a new width and release to keep it for that list.
   Opening a panel, entering text input, changing lists, or quitting before release cancels the preview.
10. Press `?` for help, Escape to close the current overlay or cancel active text input, and `q` or Ctrl+C to quit normally.
   Quit, `:quit`, terminal Ctrl+C, and handleable platform signals share one graceful exit path: they retire scan and editor presentation and unfinished input, then leave the loop.
   Quitting while a save is still in flight waits for that write instead of leaving immediately: the status row says the save is finishing, other keys do nothing, and Ctrl+C stops waiting.

These are the shipped shortcuts.
The TUI loads global overrides from the `shortcuts` group in `<config>/tui.yaml`; supported changes update both behavior and the key shown in status chips, panels, Help, and the Command Palette.
An empty chord list unbinds a configurable action.
The TUI currently has no shortcut editor and does not rewrite that group on ordinary exit, so edit the YAML only while Aobus is not running and use the stable action ids and chord syntax in the [keyboard map reference](../reference/shell/keymap.md).
Ctrl+C, text-entry editing/submission/cancellation keys, overlay navigation/activation/Escape, notification `x`, and mouse input remain fixed protocol and cannot be disabled by a root shortcut override.

The default session file is `<root>/.aobus/tui-workspace.yaml` unless `--config` selects another path.
On startup, the TUI restores its open track views, active view, filters, presentations, custom presentation presets, and last restorable playback subject, position, modes, volume, and mute from this file.
A restored playback subject remains idle until you press Space or otherwise start playback.
When several filtered views use the same list, the previously active one is restored exactly.

Ctrl+S in the Track Properties editor is not a preference save: it writes track metadata and tags into the library for every captured target, and nothing about it is deferred to quit.
Column widths and preferred presentations are the other kind of save, kept per list in the layout file below and written when you release a drag or when the shell checkpoints its state.

Per-list column layouts and preferred presentations are stored separately in `<root>/.aobus/tui_layout.yaml`.
Column widths in that file are terminal cells; fixed widths are projected within the supported 8-through-160-cell range, while flexible columns reflow when the terminal size changes. The TUI does not reuse GTK desktop widths.
Opening a list uses its remembered presentation, while startup still keeps the exact presentation of the restored active view.

Normal quit retires an in-flight scan and an open editor without presenting a late outcome, waits for an already submitted metadata write, cancels unfinished input and pointer interactions, saves committed layout/presentation preferences plus workspace and playback state, and then stops playback.
Track selection, an unfinished Quick Filter draft, open panels, pointer state, and an unfinished column-width preview are not restored.
The output device you select is remembered separately as a global TUI preference in `<config>/tui.yaml` rather than in the per-library session file; saving it preserves the load-only `shortcuts` sibling.

## Verify the result

- The library and track rows correspond to the requested root.
- `:scan` posts scan progress and a finished outcome; `:scan cancel` stops a running scan without treating cancellation as a failed scan.
- `e` over a marked set opens one editor titled with that count, and Ctrl+S reports the same number of updated tracks once every target actually changed.
- Reopening the editor, or the detail pane, shows the values you applied.
- Playing a selection updates the one-row playback dock and seek rail.
- A filter changes the visible projection, and `c` clears it.
- Opening the detail pane leaves the track table usable, and the pane's contents change as the selection moves.
- Opening any other overlay prevents workspace-only gestures from mutating the track table beneath it.
- Restarting the TUI returns to the active filtered/presentation view, restores committed per-list columns and presentation preferences, and exposes the saved playback subject without starting audio automatically.

## Related documents

- [TUI command reference](../reference/tui/command.md)
- [Keyboard map reference](../reference/shell/keymap.md)
- [TUI interaction specification](../spec/tui/interaction.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Track preset reference](../reference/presentation/track-preset.md)
