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
Initialize the root with the GTK application or `aobus -C /music init` first. After opening the TUI, run `:scan` to index its audio files; see [Use the CLI](use-cli.md) for the shell workflow.

## Steps

1. Start the TUI for the indexed root:

   ```bash
   aobus-tui --library /music
   ```

2. Move the cursor with Up/Down, PageUp/PageDown, Home, and End.
   Lists and presentation pickers also accept j/k. Press `/` inside either to search, then Enter to open a match. Escape clears search first. Help, Quality, and Notifications scroll with the same navigation keys; pages follow the visible terminal height.
3. Press Enter to play the focused track.
   Use Space for play/pause, `s` to stop, `[` and `]` to seek by five seconds, and `-`/`+` to change volume by five percentage points.
4. Press `/` to open Quick Filter in the bottom status bar, then type to filter the current view live; suggestions open directly above the input.
   Up/Down selects a suggestion, Tab accepts it while you keep editing, and Enter accepts it and closes Quick Filter.
   Escape keeps the text you typed instead of the selected suggestion.
   To clear the current filter, press `C` in the workspace or press `/` followed immediately by Enter; `/` followed immediately by Escape preserves it.
5. Press `:` to open the Command Palette, then enter a command such as:

   ```text
   :filter $composer == "Bach"
   :view classical-works
   ```

   You can also search action names in the current UI language: type `set` (or `设置` in Chinese) and press Enter to open Settings. Each action appears once, even when it has several command aliases. Tab fills the selected candidate; Enter runs it. A complete typed command keeps priority until you move the candidate selection.
   In command and filter input, Left/Right moves the caret, Home/End moves to the boundaries, and Backspace/Delete edits around it. Alt+B/F moves by word; Ctrl+W removes the preceding word. Ctrl+U/K removes text before/after the caret. Ctrl+P/N recalls earlier entries from separate session histories and can return to your unfinished draft.
   `:scan` and `:rescan` start an eager library scan; `:scan cancel` requests cooperative cancellation.
   A scan already in progress, or a cancellation still settling, posts a short notice instead of starting a second flight.
   Progress uses the existing status line; the finished scan uses the same outcome sentence as the other shells.
   `m` marks or unmarks the focused track without moving the cursor. `v` starts a vim-style visual selection at the cursor: move with `j`/`k` or the arrow keys and the range grows as you go, `v` again keeps it, Escape throws it away and restores the marks you had before. While the selection runs the status line shows `VISUAL` in front of the counts, and with Tracks focused, Escape cancels it even with the detail pane open, which stays open. If Detail has focus, the first Escape returns to Tracks and preserves the range. `Shift+A` marks every track in the current view, including rows off screen. `u` clears marks so the effective selection falls back to the focused track.
   A marked row reverses its own foreground and background, so it follows your terminal color scheme instead of a fixed color; the cursor row reverses its yellow highlight the same way when it is marked.
   Playback, Detail, and cover art still follow the cursor. Enter plays the focused track even when other rows are marked. When marks exist, the status line shows how many tracks are marked.
   Opening a different List or applying a new filter clears marks and returns the cursor to the top; confirming the current List or reloading keeps both the marked ids that are still in the view and the cursor.
6. For a quick tag change, press `t` or run `:tags`.
   The centered popover edits marked tracks, or just the focused track when nothing is marked, and shows the target count.
   Type to search or name a new tag, use Up/Down to select a result, and Space to toggle it.
   While typing a query, spaces remain part of the name.
   Enter applies pending changes and creates the offered name while typing a query, even when existing tags partially match it.
   With results focused, a new tag is created only when its creation row is selected.
   Escape or an outside click cancels the draft immediately.
   For the full editor, press `e`, `:edit`, or `:properties` to open the Track Properties editor over the current selection: every marked track, or the focused track when nothing is marked.
   The editor appears as a centered modal dialog floating over the dimmed workspace. Its target list is frozen when it opens, so editing a different set of tracks means closing it and opening a new one.
   Press Tab or Shift+Tab to switch between the editor's pages: `Metadata`, `Tags`, `Properties` (read-only audio/technical details), and (for multi-track selections) `Tracks` (to review captured titles and paths).
   On Properties and Tracks, j/k or Up/Down scrolls rows; PageUp/PageDown moves by a viewport and Home/End goes to the beginning/end.
   On the `Metadata` page, editing is direct without checkboxes: Up and Down move between rows, and typing immediately edits the focused field.
   An edited field displays a passive changed marker (`*`), while an active indicator (`>`) marks the focused row.
   Only changed fields are written to every target; untouched fields preserve each track's existing value.
   A field shared across all targets shows its value, while differing fields display a multiple-values placeholder; typing there replaces that field for all targets.
   To clear a field across all targets, press Ctrl+D; pressing Ctrl+G restores the field to its baseline value or mixed preservation state.
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
7. Click the title, artist, or album in the playback bar to browse from the playing track. The title locates the track, the artist opens matching tracks across the library, and the album locates the track in album groups. Playback continues while you browse.
   Press `g` to open Go to: `t` locates the playing track, `a` opens its artist, and `b` opens its album. `g [` goes back through views and filters; `g ]` goes forward. Plain `[`/`]` still seeks playback, while `{`/`}` moves between groups. The `g` prefix gives navigation a shared entry point, and paired brackets express direction.
   After `g`, the bottom bar switches to these suffixes and Escape to cancel. Its targets are clickable, unavailable destinations are dimmed, and narrow terminals shorten labels while retaining the keys. The menu also accepts arrows or j/k and Enter; it waits without a timeout. Escape or an unrelated key closes it without running a workspace action. Executing or cancelling restores the ordinary status hints.
   `c` remains a direct shortcut to the playing track. `:current`, `:artist`, `:album`, `:back`, and `:forward` offer the same destinations; `:goto` opens the menu.
   Use `l` to access Lists and `L` to toggle the pinned Lists pane; toggle panels with `d` for detail, `a` for the quality pipeline, `o` for output devices, `p` for presentations, and `n` for notifications.
8. Press `d` and keep browsing: the detail pane stays open beside the track table and follows the cursor, so arrows, pages, wheel, scrollbar, group jumps, playback, and filtering all keep working while you read it.
   Press `d` again or click `›` in the divider to hide it; click `‹` on the right edge to show it again. The pane has one-cell inner padding and remains open when you use popovers or editors. Escape closes those temporary surfaces without hiding Detail.
9. With mouse tracking enabled, drag a track-header column edge to preview a new width and release to keep it for that list.
   Opening a panel, entering text input, changing lists, or quitting before release cancels the preview.
10. Press `?` or F1 for centered Help. Its body scrolls while the title and footer stay visible. Press the Help shortcut again or click outside to close it without changing the underlying workspace. Press Escape to close the current overlay or cancel active text input, and Shift+Q (uppercase `Q`) or Ctrl+C to quit normally.
   Plain `q` has no default action, reducing accidental exits; explicit custom quit bindings remain effective.
   Shift+Q, `:quit`, terminal Ctrl+C, and handleable platform signals share one graceful exit path: they retire scan and editor presentation and unfinished input, then leave the loop.
   Quitting while a save is still in flight waits for that write instead of leaving immediately: the status row says the save is finishing, other keys do nothing, and Ctrl+C stops waiting.

Help groups shortcuts by navigation, browsing, playback, selection and editing, panels, and application actions.

These are the shipped shortcuts.
The TUI loads global overrides from the `shortcuts` group in `<config>/tui.yaml`; supported changes update both behavior and the key shown in status chips, panels, Help, and the Command Palette.
An empty chord list unbinds a configurable action.
Press `,` or click Settings at the bottom right, then choose Keyboard to edit shortcuts live. Left/Right selects one chord; `a` or Insert adds, Enter replaces, Delete removes it, and `r` restores that action's defaults. Conflicts and unsupported terminal chords are rejected. Ordinary exit does not rewrite untouched shortcuts; see the [keyboard map reference](../reference/shell/keymap.md).
Ctrl+C, text-entry editing/submission/cancellation keys, overlay navigation/activation/Escape, notification `x`, and mouse input remain fixed protocol and cannot be disabled by a root shortcut override.

The default session file is `<root>/.aobus/tui-workspace.yaml` unless `--config` selects another path.
On startup, the TUI restores its open track views, active view, filters, presentations, custom presentation presets, and last restorable playback subject, position, modes, volume, and mute from this file.
A restored playback subject remains idle until you press Space or otherwise start playback.
When several filtered views use the same list, the previously active one is restored exactly.

Ctrl+S in the Track Properties editor is not a preference save: it writes track metadata and tags into the library for every captured target, and nothing about it is deferred to quit.
List panel visibility and sidebar widths, plus per-List column widths and preferred presentations, are kept in the layout file below and written when you release a drag or when the shell checkpoints its state.

List panel visibility and sidebar widths, plus per-List column layouts and preferred presentations, are stored separately in `<root>/.aobus/tui_layout.yaml`.
Column widths in that file are terminal cells; fixed widths are projected within the supported 8-through-160-cell range, while flexible columns reflow when the terminal size changes. The TUI does not reuse GTK desktop widths.
Opening a list uses its remembered presentation, while startup still keeps the exact presentation of the restored active view.

Normal quit retires an in-flight scan and an open editor without presenting a late outcome, waits for an already submitted metadata write, cancels unfinished input and pointer interactions, saves committed layout/presentation preferences plus workspace and playback state, and then stops playback.
Track selection, an unfinished Quick Filter draft, open panels, pointer state, and an unfinished column-width preview are not restored.
The output device you select is remembered separately as a global TUI preference in `<config>/tui.yaml` rather than in the per-library session file; saving it preserves the `shortcuts` and `preferences` siblings.

## Understand the current workspace

An empty track table explains whether the current List is empty, a filter has no matches, or a filter is invalid. An empty All Tracks view offers `:scan`; an unsuccessful search does not ask you to initialize the library again. These messages wrap in narrow terminals.

The bottom bar advertises play and filter actions when space permits. While a filter is present, its text and clear control take priority over secondary shortcuts. Long filters end in an ellipsis; click the filter or use its shortcut to enter Quick Filter, and use Ctrl+P to recall a previous entry. A `!` before the filter text marks an invalid draft: edit it or clear it to recover.

Quick Filter explains the current Enter/Escape transitions: an empty draft offers clearing or preserving the filter; literal text without suggestions can still be applied. At narrow widths, these instructions take priority over the history reminder. During a visual range, the bottom bar instead offers keeping the marks or cancelling the range, including while Detail is open with Tracks focused. With Detail focused, the first Escape returns to Tracks and preserves the range. Both controls are clickable.

In a narrow filtered workspace, informational notices yield their space to the filter. Warnings and errors retain a clickable `!`; running work retains `…`. Click that indicator or open Notifications to read the message. The playback bar shortens its seek rail before sacrificing the track title, elapsed time, or complete volume value.

Lists has a pinned tree pane and a separate list chooser popup. Press `l` to focus the pinned pane; when it is absent, `l` opens or closes the popup. Press `L` or use `:sidebar` to toggle pinning independently. In the chooser, `L` closes the popup and shows the pinned tree when enough width is available. Tab or Shift+Tab switches between pinned Lists and Tracks.

The divider has a centered `‹` control to unpin Lists. When there is room to pin them again, the track panel's left border shows `›` to restore the pane. The track panel's bottom border shows a **List · current list** button whenever the pinned pane is absent. Clicking it opens the chooser immediately above the button. The button is hidden while the tree is docked and returns when unpinned or when the terminal becomes too narrow. Pinning is remembered across restarts. Opening or dismissing a popup never changes that preference. The pinned pane shows the selected list's filter expression as dim text at the bottom, without a heading or separator. It wraps into at most two lines and marks truncation with an ellipsis; lists without an expression reclaim that space. Selection, not mouse hover, chooses the expression. Focus-switch hints appear in the global status bar in both directions: Tracks shows `Tab lists` while the pane is docked, and Lists shows `Tab tracks`. They follow custom key bindings.

Drag either sidebar divider away from its centered arrow to resize that pane. The divider highlights while dragging; release to save, or press Escape to cancel. Both single and double borders support this. For quick keyboard resizing, focus Lists with `l` or Detail with `D`, then press Shift+Left/Right to move that divider one terminal cell in the indicated screen direction. For Detail, moving left makes it wider. The status bar shows these keys while the sidebar has focus. Width changes retain room for Tracks; narrowing the terminal clamps the displayed widths without overwriting your preferences, which return when space permits.

Press `Ctrl+W` from Tracks, Lists, or Detail to adjust the layout without changing workspace focus. The active divider is highlighted: Lists starts at its right edge, Detail at its left edge, and Tracks starts at the left divider (or the right one if it is the only visible divider). Tab or Shift+Tab switches between visible dividers. Left/Right or `h/l` moves the selected divider one cell in screen direction. Enter or the resize shortcut again saves all previewed widths together; Escape cancels the whole adjustment. A mouse click cancels and is consumed; terminal size changes also cancel. With no visible dividers the shortcut does nothing. The bottom bar shows the target and current controls, and the entry shortcut can be changed under Settings → Keyboard. Text inputs retain Ctrl+W word deletion.

Up/Down or j/k moves the List cursor. In the pinned pane, Left/Right expands or collapses the tree. Enter opens a List and returns to Tracks. The pinned tree shows its search row only after pressing `/` or clicking `/ Search` in the bottom bar while Lists has focus. Escape clears the search and removes the row. Search matches names and ancestor paths in the pinned tree, or the displayed list labels and expressions in the chooser. Escape clears search first, then returns to Tracks; in a popup it also dismisses the surface. In the pinned pane, Tab ends search and returns to Tracks. Clicking a pinned List keeps Lists focused; clicking a popup row opens it and dismisses the popup. Clicking outside a popup dismisses it without acting on the track underneath. Clicking the active List preserves its filter and selection.

Use `c` to return to the currently playing track, even after filtering it out or browsing another List. The previous view remains available through `:back`; `:forward` returns to the reveal destination. Use uppercase `C` to clear the current filter.

`<` and `>` change the playing track; Up/Down and `j/k` move the table focus. Left/Right seek, Space pauses/resumes, `S` toggles shuffle, `r` cycles repeat, and `R` reloads the List. The playback bar shows `⇄` for shuffle and `↻` / `↻1` for repeat-all / repeat-one; dim indicators mean off. Playback controls remain available while browsing panels, except while typing or editing.

### Operate detail sections

`d` shows or hides Detail while you continue choosing tracks. Press `D` to open and focus its sections explicitly; Detail stays outside the ordinary Lists/Tracks Tab cycle.

Settings → Appearance → **Reveal indicators on hover** hides the Lists and Detail arrows until the pointer enters their divider or collapsed side border. Moving away restores a continuous border. This is off by default; turning it on keeps the same click targets and keyboard controls.

Within Detail, `j/k` or Up/Down select a section, Enter toggles it, Left/Right collapse or expand it, and PageUp/PageDown scroll long contents. Escape returns to Tracks without hiding the sidebar. Space still controls playback. The bottom bar shows these controls while Detail has focus.

Metadata starts expanded; Audio Properties starts collapsed with a compact summary. Their expansion states survive changing tracks during the session. Clicking a section header toggles it without taking keyboard focus from Lists or Tracks. Tags remain read-only.

Fields use aligned label and value columns, including title, artist, album, year, track number, duration, and audio properties. Long values continue in the value column; missing fields are omitted.

## Mouse controls

Enable Mouse control under Settings → Interaction. With it enabled:

| Surface | Interaction |
|---|---|
| Status bar | Click a visible shortcut chip to open or run its action. |
| Tracks | Click to focus and clear marks; double-click to play. Ctrl+click toggles a mark. Shift+click starts or extends a visual range; Escape cancels it and `v` confirms it. |
| Track table | Wheel moves focus; drag the scrollbar to jump. Click a section heading to focus its first track; drag a column edge to resize. |
| Shuffle / Repeat | Click `⇄` to toggle shuffle. Click `↻` to cycle repeat: off → all → one → off. Active modes are highlighted; `↻1` means repeat one. |
| Volume | Wheel changes the level using the configured volume step. Click to mute or restore sound; muted output displays a localized mute label. |
| Lists, Presentation, Output | Click a row to activate it; wheel moves the highlighted row. |
| Command and filter suggestions | Click a candidate to accept it. A complete command runs immediately; a command prefix or Quick Filter stays open for more input. Wheel moves the highlight. |
| Settings | Click tabs and rows. Click `<` or the value/`>` to adjust; choose a language to save it. Wheel navigates without changing values. Click a shortcut chord to replace it; the footer offers add, remove, restore, and confirmation controls. |
| Track Properties | Click tabs, fields, suggestions, and tags. Click within an editable value to place the cursor. Wheel navigates rows; footer controls apply, reload, clear, restore, or close using the same confirmation rules as the keyboard. |
| Detail, Quality, Help, Notifications | Wheel scrolls the panel. Click a dismissible notification to hide it locally. |
| Popovers | Click outside or press `Esc` to close; the outside click does not activate the workspace beneath. |
| Settings and full Track Properties | Click `×` to use their Escape/close behavior, including unsaved-change confirmation. |

Click outside the Lists chooser or a Presentation, Output, Quality, Notifications, or Help panel to close it. That click is consumed, so it cannot also play a track or activate a background control. Clicking outside the Command Palette cancels its draft; clicking outside Quick Filter keeps your typed text, just like Escape. The Detail sidebar uses its divider arrow or `d`; editor dialogs use their close controls.

Mouse modifiers must be forwarded by your terminal; some terminals reserve Shift for selecting terminal text. Disabling Mouse control also disables clicks inside Settings, so use the keyboard to re-enable it.

## Settings

If saved preferences are invalid or unreadable, TUI starts with defaults and shows a warning. The original preference group is retained until an explicit successful Settings save.

Press `,` in the workspace or click Settings at the bottom right. `:settings` and `:config` also work. The status bar and Help show your current shortcut; the Settings button remains available if you unbind it. Text input keeps treating a comma as text. Tab and Shift+Tab switch between General, Appearance, Interaction, and Keyboard. Up/Down selects an item; Escape closes unless a failed save is pending, in which case it asks for confirmation before discarding that attempt. Escape inside the language chooser or shortcut capture only cancels that unconfirmed choice.

In Settings Keyboard, press `/` to find an action by its translated name or action id, then Enter to edit its shortcut. Escape clears the search first; Tab switches pages and clears it.

General includes system language, English, Deutsch, Español, Français, 日本語, 简体中文, and 繁體中文. Confirming a language updates the open Settings dialog and workspace immediately, including localized headings and browsing/completion ordering. Focus and marks remain on the same tracks; playback and its captured sequence continue. Existing literal notification messages retain their original wording; structured notifications and new scan results use the selected language. Close Track Properties before opening Settings.

Appearance controls the dimmed modal backdrop, reduced motion, cover renderer, and panel separation. Choose **Single │** for a shared border or **Double ││** for adjacent panel borders; this applies to pinned Lists and Detail immediately and is remembered across launches. Each pane retains its inner padding. Hovering a divider highlights its lines and centered arrow; only clicking the arrow collapses the pane. Dimming uses your terminal's dim attribute, so its strength depends on the terminal theme. Reduced motion uses a static frame for time-driven decoration and pauses the soul animation; the playhead keeps moving. A command-line cover-mode override stays effective for this session and is shown beside the preference. Editing that preference still saves the renderer for future launches without the override.

Interaction controls mouse input, wheel movement (1–10 tracks), keyboard seek (1–60 seconds), volume steps for keyboard and wheel (1–10 percentage points), and the quality hover popup. Preference and shortcut changes save immediately. On failure the applied setting stays unchanged; the dialog keeps the attempted value visible and offers Ctrl+R to retry or Ctrl+G to discard.

## Verify the result

- The library and track rows correspond to the requested root.
- `:scan` posts scan progress and a finished outcome; `:scan cancel` stops a running scan without treating cancellation as a failed scan.
- `e` over a marked set opens one editor titled with that count, and Ctrl+S reports the same number of updated tracks once every target actually changed.
- Reopening the editor, or the detail pane, shows the values you applied.
- Playing a selection updates the one-row playback dock and seek rail.
- A filter changes the visible projection, and `C` clears it.
- Opening the detail pane leaves the track table usable, and the pane's contents change as the selection moves.
- Opening any other overlay prevents workspace-only gestures from mutating the track table beneath it.
- Restarting the TUI returns to the active filtered/presentation view, restores committed per-list columns and presentation preferences, and exposes the saved playback subject without starting audio automatically.

## Related documents

- [TUI command reference](../reference/tui/command.md)
- [Keyboard map reference](../reference/shell/keymap.md)
- [TUI interaction specification](../spec/tui/interaction.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Track preset reference](../reference/presentation/track-preset.md)
