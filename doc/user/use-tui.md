---
id: user.use-tui
---

# Use the terminal frontend

## Outcome

You can open an Aobus library, browse and play tracks, filter the current view, edit selected tracks, and inspect the main panels from a terminal.

## Before you start

The TUI can create an empty database when opening a root, but it does not automatically index music. To prepare an indexed library first, use the Linux GTK application, Windows desktop, or:

```bash
aobus -C /music init
```

`aobus init` performs the initial scan. You can also scan from the TUI with `:scan`, both for initial indexing and after files change.

## Open and browse a library

1. Start the TUI for the intended root:

   ```bash
   aobus-tui --library /music
   ```

2. Move through tracks with Up/Down, PageUp/PageDown, Home, and End. The shipped `j`/`k` shortcuts also move one row.
3. Press Enter to start playback from the focused track. Space pauses or resumes the current track, even if you move the cursor.
   With nothing to resume, Space starts playback only when the cursor is on a track, regardless of which rows are marked. [Linux system Play](#use-linux-system-media-controls) uses the same rule to resume or start a track.
   Use `s` to stop, `-`/`+` to change volume, and `<`/`>` for the previous or next track in the playback sequence.
   Press Left or `[` to seek back, or Right or `]` to seek forward. The default step is five seconds; change it under **Settings → Interaction → Keyboard seek step**.
   If a track's total shows `--:--`, you can still read its elapsed time, but you can't seek.
4. Press `l` to focus pinned Lists or open the List chooser. Press uppercase `L` to pin or unpin the Lists pane. Use `p` for presentations.
5. Press `?` or F1 for the effective shortcut list. Shortcuts can be changed, so Help is the authority for the current session.

The current presentation determines visible and playback order. Up/Down changes table focus; it does not skip the playback sequence unless you press Enter.

**Manual Order** is unavailable in **All Tracks**. To arrange the full library manually, open a saved root List with an empty filter, press `p`, and choose **Manual Order**. With no tracks marked, focus a track and run `:order top` to move it to the top of the List.

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
:filter $composer = "Bach"
:view classical-works
:scan
:scan cancel
```

You may type commands with or without the leading colon. Command names are case-insensitive; filter and presentation arguments retain their text. Enter runs a complete typed command or the selected action, while Escape discards the command draft.

Useful aliases include `:lists`, `:detail`, `:quality`, `:output`, `:views`, `:notifications`, `:settings`, `:edit`, `:tags`, `:current`, `:back`, `:forward`, and `:quit`. See the [TUI command reference](../reference/tui/command.md) for the complete aliases, keys, and fixed input protocol.

`:scan` starts an eager scan: it requests audio identification during the scan rather than deferring it. `:scan cancel` asks the scan to stop. There is no separate notice when cancellation finishes. If `:scan` reports that a scan is running or cancellation is in progress, no restart is queued. Run `:scan` again after the current scan stops.

## Select and edit tracks

The focused track is the effective selection when no rows are marked.

- `m` marks or unmarks the focused row.
- `v` starts or confirms a visual range; move the focus to extend it and press Escape to cancel.
- Shift+A marks every track in the current view; `u` clears marks.
- Enter always plays the focused row, even when other rows are marked.

Press `t` or run `:tags` for a quick tag edit over all marked tracks, or the focused track when nothing is marked. Type to search or name a tag, use Up/Down to select it, Space to change its pending state, and Enter to apply. Escape or an outside click discards the draft.

Press `e`, run `:edit`, or run `:properties` for the full Track Properties editor.
It edits only the tracks marked when you open it, or the focused track if none are marked.

- Tab/Shift+Tab changes between Metadata, Tags, Properties, and the multi-selection Tracks page, except while the Credits child uses them to select its controls.
- Typing edits the focused metadata field. Ctrl+D clears it for all tracks being edited; Ctrl+G undoes edits to that field since the editor opened or was last successfully reloaded.
  If the last loaded values differed, Ctrl+G restores `<Multiple Values>` without replacing any track's value.
- On Tags, Enter cycles the selected tag's intended result.
- Enter on a Conductor, Ensemble, or Soloist preview opens that category's Credits editor; Ctrl+O on Metadata opens all kinds. Previews are read-only first-name `+N` summaries, not scalar inputs.
- In Credits, Tab selects Name, Role, Kind, Add/Delete, Move, Replace Scope, Clear, Commit, or Cancel; Enter activates the control. Kind is locked in a category editor, and moves stay within a kind. An empty list starts on Add, and deleting its last row returns there.
- Mixed scopes require explicit replacement or the separate Clear action. Ctrl+S accepts the child into Properties; Escape cancels just the child.
- After finishing the child, Ctrl+S submits metadata, staged Credits, and tag changes together. The active child blocks parent Save.
- Ctrl+R reloads the latest values for the same tracks. Escape closes the editor. Either action asks before discarding unsaved changes.

If the library changes while the editor is open, the draft becomes stale and cannot be saved. Reload it with Ctrl+R or close it and begin again. Input is inert while a submitted write is finishing.

## Inspect playback and panels

Use these shipped shortcuts:

| Key           | Surface                                                  |
| ------------- | -------------------------------------------------------- |
| `d`           | show or hide track Detail                                |
| uppercase `D` | show and focus Detail                                    |
| `a`           | audio quality pipeline                                   |
| `o`           | output devices                                           |
| `p`           | presentations                                            |
| `n`           | notifications                                            |
| `g`           | Go to current track, artist, album, or workspace history |
| `c`           | reveal the playing track directly                        |

Press `c` to return to the playing track in the List where playback started, or **All Tracks** if it no longer belongs to that List. If reveal changes views, run `:back` once to return with your filter intact.

On Linux, press `o` to choose a named PipeWire device. When you reopen **Output Devices**, the picker highlights the current device if it is listed. If your requested device is unavailable and awaiting discovery, no row is highlighted, so pressing Enter alone does nothing.

By default Aobus won't play on another output if your named device is missing. To allow rerouting, navigate to **Let system use other outputs** and press Enter, or click the checkbox. See [what happens if a PipeWire device disappears](play-music.md#choose-what-happens-if-a-pipewire-device-disappears) before enabling it in **Exclusive Mode**.

Detail stays beside the track table while you browse. Escape closes temporary overlays but does not hide Detail.
Credits show complete names, kinds, and optional roles. With Detail focused, Ctrl+K opens all Credits for the current selection: every marked track, or the focused track when none is marked. `1` through `9` open one of the first nine kinds, Conductor through Engineer, for the same tracks; edit Mixer, DJ-mixer, and Remixer with Ctrl+K.
Table category columns stay compact and count duplicate entries; detail and editor show the complete lists. The quality conclusion describes the active playback path, not metadata stored on the track. See [interpreting playback quality](play-music.md#inspect-playback-quality) for source fidelity, pipeline changes, and incomplete evidence.

The playback bar's four-character mode combines sequential or shuffled order with repeat state. Clicking it advances through presets; `S` changes shuffle and `r` cycles repeat independently. The [TUI command reference](../reference/tui/command.md#playback-mode-codes) defines the codes.

## Use Linux system media controls

A Linux TUI built with system-media support joins the current session bus by default and appears as **Aobus TUI**. Each running TUI gets a distinct MPRIS name, so several libraries can remain controllable independently. The TUI has no desktop entry and does not offer the MPRIS Raise action.

Use `--system-media=off` when the session must not create the MPRIS adapter, its private native thread, or artwork interests:

```bash
aobus-tui --library /music --system-media=off
```

When launching through native Bazel from a prepared source checkout, put
runtime options after `--`:

```bash
bazel run //app/tui:aobus-tui -- --library /music --system-media=off
```

The default `auto` mode is best effort. It remains inert when the binary lacks Linux system-media support or no usable session bus exists; Aobus does not autolaunch or reconnect to a bus. The option is accepted on Windows and macOS, but those TUI builds currently provide no native system-media integration. An external Quit request follows the normal safe-exit path described below.

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

Settings include language, cover rendering, reduced motion, panel borders, mouse and step sizes, terminal title behavior, and shortcuts. Changes save immediately. If a save fails, the running value remains unchanged and the page offers Ctrl+R to retry or Ctrl+G to discard the unsaved change.

On Keyboard, Left/Right selects a chord, `a` or Insert adds one, Enter replaces one, Delete removes one, and `r` restores that action's defaults. Unsupported terminal chords and conflicts are rejected. Fixed text editing, modal navigation, Escape, and Ctrl+C cannot be replaced by root shortcut overrides.

## Quit safely

Press uppercase `Q`, run `:quit`, or press Ctrl+C. Normal exit—including an accepted Linux MPRIS Quit—retires an active scan, editor presentation, and system-media command admission, saves committed workspace and layout state, and stops playback. If a metadata write has already been submitted, the TUI waits for it; Ctrl+C stops waiting.

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
- [Linux MPRIS behavior](../system/frontend/mpris.md)
- [TUI track authoring](../system/frontend/tui-track-authoring.md)
- [Edit metadata and Credits](edit-metadata.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Track preset reference](../reference/presentation/track-preset.md)
