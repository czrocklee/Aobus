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
4. Press `/` or `:` to enter command mode.
   Submit ordinary text for a quick filter, or use a command such as:

   ```text
   /filter $composer == "Bach"
   /view classical-works
   ```

5. Toggle panels with `l` for lists, `d` for detail, `a` for the quality pipeline, `o` for output devices, `v` for presentations, and `n` for notifications.
6. Press `?` for help, Escape to close the current overlay or cancel command mode, and `q` or Ctrl+C to quit.

The default workspace configuration is `<root>/.aobus/tui-workspace.yaml` unless `--config` selects another path.

## Verify the result

- The library and track rows correspond to the requested root.
- Playing a selection updates the one-row playback dock and seek rail.
- A filter changes the visible projection, and `c` clears it.
- Opening an overlay prevents workspace-only gestures from mutating the track table beneath it.

## Related documents

- [TUI command reference](../reference/tui/command.md)
- [TUI interaction specification](../spec/tui/interaction.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Track preset reference](../reference/presentation/track-preset.md)
