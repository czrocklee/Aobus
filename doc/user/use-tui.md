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

The TUI opens an Aobus database; it does not replace the library initialization and scan workflow.
Initialize and scan the root with the GTK application or CLI first.

## Steps

1. Start the TUI for the indexed root:

   ```bash
   aobus-tui --library /music
   ```

2. Move selection with Up/Down, PageUp/PageDown, Home, and End.
3. Press Enter or `p` to play the selected track.
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
6. Toggle panels with `l` for lists, `d` for detail, `a` for the quality pipeline, `o` for output devices, `v` for presentations, and `n` for notifications.
7. Press `d` and keep browsing: the detail pane stays open beside the track table and follows your selection, so arrows, pages, wheel, scrollbar, group jumps, playback, and filtering all keep working while you read it.
   Press `d` again or Escape to close it.
8. With mouse tracking enabled, drag a track-header column edge to preview a new width and release to keep it for that list.
   Opening a panel, entering text input, changing lists, or quitting before release cancels the preview.
9. Press `?` for help, Escape to close the current overlay or cancel active text input, and `q` or Ctrl+C to quit normally.

The default session file is `<root>/.aobus/tui-workspace.yaml` unless `--config` selects another path.
On startup, the TUI restores its open track views, active view, filters, presentations, custom presentation presets, and last restorable playback subject, position, modes, volume, and mute from this file.
A restored playback subject remains idle until you press Space or otherwise start playback.
When several filtered views use the same list, the previously active one is restored exactly.

Per-list column layouts and preferred presentations are stored separately in `<root>/.aobus/tui_layout.yaml`.
Column widths in that file are terminal cells; fixed widths are projected within the supported 8-through-160-cell range, while flexible columns reflow when the terminal size changes. The TUI does not reuse GTK desktop widths.
Opening a list uses its remembered presentation, while startup still keeps the exact presentation of the restored active view.

Normal quit cancels unfinished input and pointer interactions, saves committed layout/presentation preferences plus workspace and playback state, and then stops playback.
Track selection, an unfinished Quick Filter draft, open panels, pointer state, and an unfinished column-width preview are not restored.
The output device you select is remembered separately as a global TUI preference rather than in the per-library session file.

## Verify the result

- The library and track rows correspond to the requested root.
- Playing a selection updates the one-row playback dock and seek rail.
- A filter changes the visible projection, and `c` clears it.
- Opening the detail pane leaves the track table usable, and the pane's contents change as the selection moves.
- Opening any other overlay prevents workspace-only gestures from mutating the track table beneath it.
- Restarting the TUI returns to the active filtered/presentation view, restores committed per-list columns and presentation preferences, and exposes the saved playback subject without starting audio automatically.

## Related documents

- [TUI command reference](../reference/tui/command.md)
- [TUI interaction specification](../spec/tui/interaction.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Track preset reference](../reference/presentation/track-preset.md)
