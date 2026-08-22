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
8. Press `?` for help, Escape to close the current overlay or cancel active text input, and `q` or Ctrl+C to quit.

The default workspace configuration is `<root>/.aobus/tui-workspace.yaml` unless `--config` selects another path.

## Verify the result

- The library and track rows correspond to the requested root.
- Playing a selection updates the one-row playback dock and seek rail.
- A filter changes the visible projection, and `c` clears it.
- Opening the detail pane leaves the track table usable, and the pane's contents change as the selection moves.
- Opening any other overlay prevents workspace-only gestures from mutating the track table beneath it.

## Related documents

- [TUI command reference](../reference/tui/command.md)
- [TUI interaction specification](../spec/tui/interaction.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Track preset reference](../reference/presentation/track-preset.md)
