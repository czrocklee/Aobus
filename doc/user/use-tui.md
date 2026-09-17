---
id: user.use-tui
---
# Use the terminal frontend

## Outcome

You can open an existing Aobus library, browse and play tracks, filter the current view, edit selected tracks, and inspect the main panels from a terminal.

## Before you start

The TUI opens an existing Aobus database; it does not create one.
Initialize and index a root first with the Linux GTK application, Windows desktop, or:

```bash
aobus -C /music init
```

`aobus init` performs the initial scan. After files change, rescan from the TUI with `:scan`.

## Open and browse a library

1. Start the TUI for the intended root:

   ```bash
   aobus-tui --library /music
   ```

2. Move through tracks with Up/Down, PageUp/PageDown, Home, and End. The shipped `j`/`k` shortcuts also move one row.
3. Press Enter to play the focused track. Use Space to pause or resume, `s` to stop, `[`/`]` to seek, `-`/`+` to change volume, and `<`/`>` for the previous or next playback track.
4. Press `l` to focus pinned Lists or open the List chooser. Press uppercase `L` to pin or unpin the Lists pane. Use `p` for presentations.
5. Press `?` or F1 for the effective shortcut list. Shortcuts can be changed, so Help is the authority for the current session.

The current presentation determines visible and playback order. Up/Down changes table focus; it does not skip the playback sequence unless you press Enter.

## Filter and run commands

Press `/` to open Quick Filter in the status bar. Typing applies the draft after a short pause.

- Up/Down chooses a suggestion and Tab inserts it without closing.
- Enter accepts the selected suggestion and closes.
- Escape closes while keeping the literal text you typed rather than the selected suggestion.
- Opening an untouched empty filter and pressing Enter clears the current filter; pressing Escape preserves it.
- Uppercase `C` clears the current filter directly.

See [filtering and suggestions](manage-library.md#understand-filtering-and-suggestions) for Quick versus Expression mode and romanized completion. Accepting a suggestion inserts its original library text; keeping a romanized draft applies that literal text instead.

Press `:` for the Command Palette. It accepts commands such as:

```text
:filter $composer == "Bach"
:view classical-works
:scan
:scan cancel
```

You may type commands with or without the leading colon. Command names are case-insensitive; filter and presentation arguments retain their text. Enter runs a complete typed command or the selected action, while Escape discards the command draft.

Useful aliases include `:lists`, `:detail`, `:quality`, `:output`, `:views`, `:notifications`, `:settings`, `:edit`, `:tags`, `:current`, `:back`, `:forward`, and `:quit`. See the [TUI command reference](../reference/tui/command.md) for the complete aliases, keys, and fixed input protocol.

`:scan` starts one eager scan. If a scan is already active, another request does not start duplicate work. `:scan cancel` requests cooperative cancellation; wait for its notice before starting another scan.

## Select and edit tracks

The focused track is the effective selection when no rows are marked.

- `m` marks or unmarks the focused row.
- `v` starts or confirms a visual range; move the focus to extend it and press Escape to cancel.
- Shift+A marks every track in the current view; `u` clears marks.
- Enter always plays the focused row, even when other rows are marked.

Press `t` or run `:tags` for a quick tag edit over all marked tracks, or the focused track when nothing is marked. Type to search or name a tag, use Up/Down to select it, Space to change its pending state, and Enter to apply. Escape or an outside click discards the draft.

Press `e`, run `:edit`, or run `:properties` for the full Track Properties editor. Its target set is captured when it opens.

- Tab/Shift+Tab changes between Metadata, Tags, Properties, and the multi-selection Tracks page.
- Typing edits the focused metadata field; Ctrl+D clears it for all targets and Ctrl+G restores its baseline.
- On Tags, Enter cycles the selected tag's intended result.
- Ctrl+S submits metadata and tag changes together.
- Ctrl+R reloads every captured target. Escape closes the editor. Either action asks before discarding a dirty draft.

If the library changes while the editor is open, the draft becomes stale and cannot be saved. Reload it with Ctrl+R or close it and begin again. Input is inert while a submitted write is finishing.

## Inspect playback and panels

Use these shipped shortcuts:

| Key | Surface |
|---|---|
| `d` | show or hide track Detail |
| uppercase `D` | show and focus Detail |
| `a` | audio quality pipeline |
| `o` | output devices |
| `p` | presentations |
| `n` | notifications |
| `g` | Go to current track, artist, album, or workspace history |
| `c` | reveal the playing track directly |

Detail stays beside the track table while you browse. Escape closes temporary overlays but does not hide Detail. The quality conclusion describes the active playback path, not metadata stored on the track. See [interpreting playback quality](play-music.md#inspect-playback-quality) for source fidelity, pipeline changes, and incomplete evidence.

The playback bar's four-character mode combines sequential or shuffled order with repeat state. Clicking it advances through presets; `S` changes shuffle and `r` cycles repeat independently. The [TUI command reference](../reference/tui/command.md#playback-mode-codes) defines the codes.

## Resize and restore the workspace

When Lists or Detail has focus, Shift+Left/Right resizes that sidebar. Press Ctrl+W for resize mode across visible dividers: Tab chooses a divider, Left/Right moves it, Enter commits, and Escape cancels.

The TUI keeps different kinds of state in separate files:

- `<root>/.aobus/tui-workspace.yaml` stores open views, the active view, filters, presentations, custom presets, and restorable playback state. Use `--config` to choose another workspace file.
- `<root>/.aobus/tui_layout.yaml` stores per-library pane visibility and widths, column layouts, and preferred presentations. `--config` does not move this file.
- `<config>/tui.yaml` stores global TUI preferences, output selection, and shortcut overrides.

A restored playback subject remains idle until you start playback. Unfinished input, marks, open panels, and unfinished pointer or resize gestures are not restored.

## Mouse controls

Enable **Mouse** under Settings → Interaction if your terminal forwards mouse input.

- Click a track to focus it; double-click to play. Ctrl+click toggles a mark and Shift+click starts or extends a visual range.
- Use the wheel in tables and panels. Drag a track-column edge to preview its width and release to save it for that List.
- Click rows in Lists, Presentation, or Output to activate them.
- Clicking outside a floating panel closes it and consumes that click, so it does not also activate the workspace below.

Some terminals reserve Shift for terminal text selection. Disabling TUI mouse input also disables clicks in Settings; use the keyboard to turn it back on.

## Settings

Press `,`, click Settings in the status bar, or run `:settings`. Tab/Shift+Tab moves among General, Appearance, Interaction, and Keyboard.

Settings include language, cover rendering, reduced motion, panel borders, mouse and step sizes, terminal title behavior, and shortcuts. Changes save immediately. If a save fails, the running value remains unchanged and the page offers Ctrl+R to retry or Ctrl+G to discard the candidate.

On Keyboard, Left/Right selects a chord, `a` or Insert adds one, Enter replaces one, Delete removes one, and `r` restores that action's defaults. Unsupported terminal chords and conflicts are rejected. Fixed text editing, modal navigation, Escape, and Ctrl+C cannot be replaced by root shortcut overrides.

## Quit safely

Press uppercase `Q`, run `:quit`, or press Ctrl+C. Normal exit retires an active scan and editor presentation, saves committed workspace and layout state, and stops playback. If a metadata write has already been submitted, the TUI waits for it; Ctrl+C stops waiting.

## Verify the result

- The track rows correspond to the requested root.
- `:scan` ends with a completion notice; cancellation is not reported as a failed scan.
- A filter changes the visible rows and uppercase `C` restores the unfiltered view.
- Reopening an edited track shows the committed values.
- Restarting restores the active saved view and playback subject without starting audio automatically.

## Related documents

- [TUI command reference](../reference/tui/command.md)
- [Keyboard map reference](../reference/shell/keymap.md)
- [TUI interaction specification](../system/frontend/tui.md)
- [TUI track authoring](../system/frontend/tui-track-authoring.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Track preset reference](../reference/presentation/track-preset.md)
