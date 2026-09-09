---
id: tui.command-surface
type: reference
status: current
domain: presentation
summary: Enumerates TUI startup options, command prefixes and aliases, keyboard shortcuts, overlays, mouse targets, and default paths.
---
# TUI command reference

## Scope and version

This reference enumerates the current Aobus TUI startup and interactive input surface.
The surface is unversioned; modal and rendering behavior belongs to the [TUI interaction specification](../../spec/tui/interaction.md).

## Code boundary

Startup option authority is `app/tui/Main.cpp`.
Command-prefix and alias authority is `Command.cpp`.
Application shortcut descriptors, TUI-local defaults, neutral-to-FTXUI translation, projected collision selection, and effective display chords belong to `Keymap.cpp`.
The replaceable plan value built there is read by `EventController.cpp` for root dispatch and by every renderer that advertises a configurable shortcut.
`EventController.cpp` separately owns fixed text-input, list, overlay, notification, mouse, and Ctrl-C protocol, and forwards graceful exit to the App-owned `ExitController`.
`LibraryScanController.cpp` owns one restartable eager scan flight.
`TrackEditController.cpp` owns editor preparation, session retention, and submission; `TrackPropertiesEditor.cpp` owns modal keys and submission prompts, while `TrackMetadataEditor.cpp` and `TrackTagEditor.cpp` own page input and rendering.

## Surface

### Startup options

| Option | Default/meaning |
| --- | --- |
| `-l, --library <root>` | music library root; normalized absolute path |
| `--database <path>` | default `<root>/.aobus/library`; normalized absolute path |
| `--config <path>` | workspace/playback-session file; default `<root>/.aobus/tui-workspace.yaml`; normalized absolute path; does not relocate the layout file and must not alias another TUI managed-state file |
| `--cover-art-mode <auto|kitty|blocks|off>` | session override of the saved cover renderer; omitted uses the preference (default `auto`) |
| `--log-level <trace|debug|info|warn|error|critical|off>` | case-insensitive runtime log level |
| `--version` | prints `Aobus TUI <version>` and exits |

Per-library List navigation visibility, column layouts, and presentation preferences always use `<root>/.aobus/tui_layout.yaml`; there is no startup override for that file. Startup rejects a `--config` path that aliases this document or the global TUI application-preference document so one `ConfigStore` remains authoritative for each physical file.
Startup also exits with a diagnostic when it cannot prepare the selected workspace configuration directory.

### Shortcut overrides

At startup the TUI loads the `shortcuts` group from the application-global `<config>/tui.yaml`, using the deliberate terminal defaults in the [keyboard map reference](../shell/keymap.md).
This source is independent of the selected library and `--config`.
The Settings Keyboard page saves accepted candidates before replacing the effective dispatch/hint plan. Ordinary shutdown does not rewrite untouched shortcuts. All global preferences share one store that preserves sibling groups.

### Settings

Press `,`, click Settings in the workspace status bar, or use `:settings` / `:config` to open Settings. The shortcut is configurable through `tui.shell.openSettings`; the mouse entry remains available when it is unbound. Help lists the effective shortcut and command aliases.
Tab/Shift+Tab selects a page; Escape closes, asking for confirmation if a failed save is pending. In the language chooser or shortcut capture, Escape cancels only the unconfirmed choice. Preference rows use Left/Right or Enter. Language uses a chooser and Enter confirmation. Keyboard uses Left/Right to select a chord, `a` or Insert to add, Enter to replace, Delete to remove, and `r` to restore defaults. Failed preference and shortcut saves offer Ctrl+R retry and Ctrl+G discard.

### Command prefixes

Commands are entered through the Command Palette (shipped root shortcut `:`) and are case-insensitive after trimming.
The parser accepts the Command Palette draft with or without its leading `:`; `/` is reserved for live Quick Filter input and is never a command prefix.

| Prefix | Action |
| --- | --- |
| `filter <text>` | quick filter |
| `presentation <id>` | set track presentation |
| `preset <id>` | set track presentation |
| `view <id>` | set track presentation |

The parser accepts only known prefixes and exact aliases. The palette also searches localized action names and aliases, showing one row per action; Return activates a highlighted search result. An unmatched draft reports an unknown command without changing the filter.

### Command aliases

| Aliases | Action |
| --- | --- |
| `lists` | enable/focus Lists or disable its visible pane |
| `detail`, `details` | open/toggle detail |
| `quality`, `audio`, `pipeline` | open/toggle quality pipeline |
| `output`, `outputs`, `device`, `devices` | open/toggle output devices |
| `views` | open/toggle presentation panel |
| `notifications`, `notification` | open/toggle notification center |
| `close`, `hide`, `esc` | close overlay |
| `help` | help |
| `current`, `now`, `reveal` | reveal current track |
| `clear` | clear filter |
| `reload`, `refresh` | reload active list |
| `scan`, `rescan` | start an eager library scan |
| `scan cancel` | request cooperative cancellation of the running scan |
| `select toggle` | mark or unmark the focused track |
| `select visual` | start a visual selection at the focused track, or confirm the running one |
| `select all` | mark every track in the current view |
| `select clear` | clear marked tracks |
| `edit`, `properties` | open the Track Properties editor over the current selection |
| `play` | play the focused track |
| `pause`, `toggle`, `space` | toggle playback |
| `stop` | stop playback |
| `previous`, `next` | play the previous/next track in the playback sequence |
| `shuffle` | toggle shuffle |
| `repeat` | cycle repeat Off → All → One → Off |
| `back`, `forward` | navigate workspace history, including the view left by reveal |
| `settings`, `config` | open global TUI Settings |
| `quit` | request normal checkpoint-and-stop exit |

### Workspace keys

The following table shows shipped defaults.
Except for rows marked **fixed protocol**, each action is configurable through its stable id in the [keyboard map reference](../shell/keymap.md), and every visible hint uses the effective projected shortcut.

| Default key | Action | Ownership |
| --- | --- | --- |
| `Up`, `Down` | previous/next track or active panel row | fixed protocol |
| `PageUp`, `PageDown` | page selection | fixed protocol |
| `Home`, `End` | first/last selection | fixed protocol |
| `Return` | play the focused track | configurable at root; Return is fixed activation inside supported overlays |
| `j` / `k` | move focus to next/previous row | configurable |
| `Space` | toggle play/pause | configurable |
| `s` | stop | configurable |
| `<` / `>`, `Ctrl+Left` / `Ctrl+Right` | previous/next playback track | configurable |
| `S` | toggle shuffle | configurable |
| `r` | cycle repeat Off → All → One → Off | configurable |
| `Left` / `Right`, `[` / `]` | seek -/+ 5 seconds | configurable |
| `{` / `}` | previous/next presentation group | configurable |
| `-` / `+` / `=` | volume -/+ 5 percentage points | configurable |
| `l` | enable/focus Lists or disable its visible pane | configurable |
| `d`, `a`, `o`, `p`, `n` | toggle corresponding panel | configurable |
| `,` | open Settings | configurable at root |
| `?`, `F1` | open/close help | configurable |
| `c` | reveal current track | configurable |
| `C` | clear filter | configurable |
| `R` | reload active list | configurable |
| `Tab`, `Shift+Tab` | switch Lists/Tracks focus when docked; return to Tracks from Lists | configurable |
| `m` | mark or unmark the focused track | configurable |
| `v` | start a visual selection at the focus, or confirm the running one | configurable |
| `Shift+A` | mark every track in the current view | configurable |
| `u` | clear marked tracks | configurable |
| `e` | open the Track Properties editor over the current selection | configurable |
| `/` | open an empty live Quick Filter input | configurable |
| `:` | open an empty Command Palette input | configurable |
| `Q` (Shift+Q) | request normal exit | configurable |
| `Ctrl-C` | graceful exit request through the App exit gate | fixed protocol |
| `Esc` | close overlay, cancel active text input, or cancel a running visual selection according to its mode | fixed protocol |

Playback shortcuts also work in browse panels after local navigation, activation, and toggle handling. Active search, shell text input, Settings, and track editors consume their own input exclusively. Rebinding a root key does not change fixed picker or editor keys. Uppercase letters in this table require Shift; canonical configuration uses `Shift+C`, `Shift+R`, and `Shift+S`.

### Quick Filter keys

| Key | Action |
| --- | --- |
| printable UTF-8 | insert at the caret |
| `Backspace`, `Delete` | remove one grapheme before/at the caret |
| `Left`, `Right` | move by grapheme; clicking input also positions the caret |
| `Home`, `End`, `Ctrl+A`, `Ctrl+E` | move to the beginning/end |
| `Alt+B`, `Alt+F`, `Ctrl+Left`, `Ctrl+Right` | move across space-delimited words |
| `Ctrl+W` | delete the preceding word |
| `Ctrl+U`, `Ctrl+K` | delete text before/after the caret |
| `Ctrl+P`, `Ctrl+N` | recall older/newer session history, restoring the unfinished draft past the newest entry |
| `Up`, `Down` | cycle completion selection |
| `PageUp`, `PageDown` | move selection by one bounded completion page |
| `Tab` | apply selected completion and keep editing |
| `Return` | apply selected completion, apply the filter immediately, and close; an untouched empty draft clears the filter |
| `Esc` | ignore selected completion, apply the literal edited draft immediately, and close; an untouched draft preserves the existing filter |

Opening Quick Filter through its effective shortcut does not copy or clear the current filter.
Confirming that untouched empty input with Return clears the filter, while closing it with Escape leaves the current filter unchanged.
After an edit, the draft also applies live following a 200-millisecond quiet interval.
The active draft replaces the bottom status bar, and its completion popup opens directly above it; the separate Command Palette remains centered.

Return and Escape intentionally differ between the two input modes.
Quick Filter edits are live, so Return accepts the highlighted value and Escape keeps the literal draft; Command Palette input has no live effect; Return executes an exact typed command, or activates the selected search result, and Escape cancels it.

### Command Palette keys

| Key | Action |
| --- | --- |
| printable UTF-8 | insert at the caret |
| `Backspace`, `Delete` | remove one grapheme before/at the caret |
| `Left`, `Right` | move by grapheme; clicking input also positions the caret |
| `Home`, `End`, `Ctrl+A`, `Ctrl+E` | move to the beginning/end |
| `Alt+B`, `Alt+F`, `Ctrl+Left`, `Ctrl+Right` | move across space-delimited words |
| `Ctrl+W` | delete the preceding word |
| `Ctrl+U`, `Ctrl+K` | delete text before/after the caret |
| `Ctrl+P`, `Ctrl+N` | recall older/newer session history, restoring the unfinished draft past the newest entry |
| `Up`, `Down` | cycle completion selection |
| `PageUp`, `PageDown` | move selection by one bounded completion page |
| `Tab` | apply selected completion and keep editing |
| `Return` | run a complete typed command; otherwise activate the highlighted candidate. Explicit candidate navigation takes priority; argument prefixes remain open |
| `Esc` | discard the command draft and close |

### Track Properties editor keys

The editor is composed as a centered modal overlay over the live workspace and consumes every event it does not use, trapping keyboard and mouse interactions while open.
It provides pages for `Metadata`, `Tags`, read-only `Properties`, and (for multi-track selections) `Tracks`.

| Key | Action |
| --- | --- |
| `Tab`, `Shift-Tab` | switch to the next or previous page (`Metadata`, `Tags`, `Properties`, `Tracks`) |
| `Up`, `Down` | move focused row; navigate completion candidates or tag rows; scroll read-only pages |
| `j`, `k` | scroll down/up on the read-only Properties and Tracks pages |
| `PageUp`, `PageDown` | move metadata/tag rows or scroll read-only pages by the visible viewport; move completion candidates by six rows |
| `Left`, `Right` | move the caret by one extended grapheme cluster (dismisses completion popup) |
| `Home`, `End`, `Ctrl-A`, `Ctrl-E` | move the caret to the start or end of the text field (dismisses completion popup); Home/End moves to the first/last row on read-only pages |
| `Alt-B`, `Alt-F`, `Ctrl-Left`, `Ctrl-Right` | move across space-delimited words in text inputs (dismisses completion popup) |
| `Ctrl-W` | delete the preceding space-delimited word |
| printable UTF-8, `Backspace` | edit focused metadata field (refreshes completion candidates); edit the always-live tag query |
| `Ctrl-N` | explicitly open metadata value completion popup for supported fields |
| `Return` | accept selected completion candidate; cycle the selected tag's intent or create the tag the query names; confirm discard or reload prompt |
| `Ctrl-U`, `Ctrl-K` | delete text before/after the caret; an unchanged mixed field keeps its values |
| `Ctrl-D` | explicitly clear the focused metadata field across all targets |
| `Ctrl-G` | restore focused metadata field or tag to its baseline value |
| `Delete` | delete forward in text inputs, including the tag query |
| `Ctrl-S` | submit unified properties patch (metadata and tags) for every captured target |
| `Ctrl-R` | re-read every captured target and replace the draft baselines |
| `Esc` | close completion popup; clear a non-empty tag query; close editor (prompts confirmation if dirty) |

With Mouse control enabled, click tabs, fields, tag rows, or completion candidates; click within a text value to place the caret.
Wheel input navigates the active page or completion.
Footer controls and `×` follow the same validation, submission, and confirmation rules as their keys.

`Ctrl-R` and `Esc` ask for confirmation while the draft is dirty; `Return` confirms, `Esc` keeps editing, and every other key leaves the question open.
`Ctrl-R` is accepted in every state but advertised only in Stale, Unavailable, and error states, where it is the way forward.
While a write is in flight the surface stays visible and inert: `Ctrl-S`, `Esc`, and every other key are consumed without effect, and there is no Cancel Save control.

### Local list navigation and search

Pickers and read-only modal pages accept Up/Down or j/k, viewport-sized PageUp/PageDown, and Home/End. Root j/k remains configurable. Text editors keep printable keys for text; Home/End belongs to the caret while a search field is active. Detail continues to follow workspace selection.

Press `/` inside Lists, Views, or Settings Keyboard to search within that panel. Up/Down and page keys navigate matches; Return activates one. Escape clears the query before closing the panel. Empty results cannot activate the previously selected row. Settings searches localized labels and stable action ids. Queries do not alter the workspace filter. Views and Settings clear their query on panel/page changes; Lists retains search while a stronger surface suspends it. Lists searches names and ancestor paths, displays context-only ancestors without activation, and uses Left/Right for tree navigation outside search. Tab/Shift-Tab is a fixed search-local exit to Tracks; a printable custom focus binding remains text during search.

Lists is a workspace pane, docked when its 26 columns leave at least 72 track-content columns after Detail. Otherwise it appears as a drawer only while Lists has focus. `l` / `:lists` enables and focuses it, opens a hidden drawer, or disables a visible pane. Escape and the focus action return to Tracks without disabling it; the close button disables it. `tui.workspace.switchFocus` defaults to Tab/Shift-Tab and cannot open a hidden drawer from Tracks. Enter returns to Tracks after navigation; a docked row click keeps Lists focused. Clicking the active List preserves its current filtered view and marks. Outside drawer clicks dismiss and consume; docked Tracks clicks operate immediately.

### Overlay-specific keys

| Overlay | Keys |
| --- | --- |
| Detail | effective toggle (default `d`), `Esc` close; every workspace key and mouse gesture below stays available while it is open |
| Pipeline | effective toggle (default `a`), `Esc` close |
| Output | effective toggle (default `o`), `Return` select, `Esc` close |
| Views | effective toggle (default `p`), `Return` select, `Esc` close |
| Notifications | effective toggle (default `n`), `x` hide compact/local entry when eligible, `Esc` close |
| Help | effective toggle (defaults `?` and F1), `Esc` close; navigation keys scroll the body within its fixed frame |

### Mouse targets

All track-table gestures below remain available while the detail inspector is open and are blocked by every other overlay. A left press outside a rendered floating menu dismisses it and consumes the click; a different trigger therefore needs a subsequent click. Command Palette and Quick Filter outside presses follow their respective Escape semantics, with the Quick Filter status-row field counted as inside. Centered Help follows the same outside-click dismissal rule. The Detail side panel and editor dialogs use their close controls.

| Target/gesture | Action |
| --- | --- |
| track-table wheel | move selection by three tracks |
| table scrollbar press/drag | map visual row to selected track |
| header column edge drag/release | preview a terminal-cell width, then persist the current list's canonical layout on release; interruption rolls back |
| group header click | select first track in section |
| seek rail press/drag/release | preview/final seek |
| shuffle indicator click | toggle shuffle off/on |
| repeat indicator click | cycle repeat off → all → one → off |
| Soul button click | toggle playback |
| Soul button hover | show quality hover panel |
| library/view/status/quality/output indicators | open corresponding panel |
| list/view/output row click | select/activate according to panel |

## Validation rules

- Command prefixes match before aliases; unknown command input remains open and reports a warning.
- Exact aliases include multi-word commands such as `scan cancel`. A `scan` completion prefix may list `scan` and `scan cancel`; a trailing space after `scan` converges to `scan cancel`. Bare `select` is not a command.
- Prefix-command arguments, including internal whitespace, are preserved byte-for-byte. Arbitrary extra spacing inside an exact alias is not rewritten into that alias.
- Live Quick Filter drafts and explicit `:filter` arguments use the shared UIModel track-filter completer.
- An explicit leading query variable produces structured query suggestions; otherwise a non-empty active term produces frequency-ranked live Quick-filter value suggestions.
- Presentation completion includes built-in and custom preset ids.
- Quick-filter values come from live titles, artist, album, album artist, genre, composer, work, and tags; list names and other fields are excluded.
- `:edit`, `:properties`, and the effective edit shortcut open one editor over the whole captured selection; with no marks that is the focused track alone.
- Opening the editor is refused with a warning when the selection is empty, when the library is changing or unavailable, or when any captured target is already gone.
- Both text-input modes and modal overlays disable workspace seek/table gestures; the detail inspector does not.
- Opening or closing an overlay, entering text input, changing lists, another pointer press, or teardown cancels an unfinished column drag without saving it.
- A duration-zero seek rail is inert.
- A supported override affects root dispatch and every configurable hint for that action; an empty sequence removes both.
- Unsupported terminal chords and later projected collisions omit only those entries, while fixed protocol and unrelated supported actions remain available.
- Fixed protocol takes precedence in its active scope, so rebinding Return, Escape, navigation, or a text-editing key cannot strand an input or modal overlay.

## Compatibility and versioning

Command aliases and fixed protocol are unversioned.
Stable `tui.*` action ids and canonical neutral chord strings are persisted compatibility surfaces; changing one requires an explicit migration decision.
Changing a default key, alias, option, or default path requires updating this reference and the relevant model/controller test.

## Examples

```text
:filter $composer == "Bach"
:view classical-works
:scan
:scan cancel
:edit
:notifications
```

## Implementation authority

- [`Main.cpp`](../../../app/tui/Main.cpp) registers startup options.
- [`Command.cpp`](../../../app/tui/Command.cpp) registers prefixes and aliases.
- [`Keymap.cpp`](../../../app/tui/Keymap.cpp) registers stable terminal action ids and defaults and owns executable projection plus dynamic shortcut selection.
- [`CommandCompletion.cpp`](../../../app/tui/CommandCompletion.cpp) routes command, presentation, and shared filter completion.
- [`EventController.cpp`](../../../app/tui/EventController.cpp) applies the prepared root plan after fixed scoped protocol and maps mouse events.
- [`LibraryScanController.cpp`](../../../app/tui/LibraryScanController.cpp) owns the single scan flight.
- [`ExitController.cpp`](../../../app/tui/ExitController.cpp) owns the idempotent graceful-exit gate.
- [`TrackEditController.cpp`](../../../app/tui/TrackEditController.cpp) owns editor preparation, the retained authoring session, and submission; [`TrackPropertiesEditor.cpp`](../../../app/tui/TrackPropertiesEditor.cpp) owns fixed modal keys and submission prompts. [`TrackMetadataEditor.cpp`](../../../app/tui/TrackMetadataEditor.cpp) owns metadata input and completion; [`TrackTagEditor.cpp`](../../../app/tui/TrackTagEditor.cpp) owns tag intents and search.

## Test authority

- [`CommandTest.cpp`](../../../test/unit/tui/CommandTest.cpp) protects commands and aliases.
- [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp) protects keyboard and mouse mappings.
- [`KeymapTest.cpp`](../../../test/unit/tui/KeymapTest.cpp) protects defaults, supported projection, terminal aliases, collisions, unbinding, and display-chord selection.
- [`CommandCompletionTest.cpp`](../../../test/unit/tui/CommandCompletionTest.cpp) protects completion routing, including multi-word exact aliases.
- [`LibraryScanControllerTest.cpp`](../../../test/unit/tui/LibraryScanControllerTest.cpp) protects scan start, cancel, and retirement.
- [`LibraryControllerTest.cpp`](../../../test/unit/tui/LibraryControllerTest.cpp) protects mark, range, select-all, and selection publication.
- [`ExitControllerTest.cpp`](../../../test/unit/tui/ExitControllerTest.cpp) protects exit phase transitions.
- [`TrackEditControllerTest.cpp`](../../../test/unit/tui/TrackEditControllerTest.cpp) protects open refusal, batch submission, staleness, reload, and a submission outliving its editor.
- [`TrackPropertiesEditorTest.cpp`](../../../test/unit/tui/TrackPropertiesEditorTest.cpp) protects modal commands, validation, and page drafts; [`TrackPropertiesEditorCompletionTest.cpp`](../../../test/unit/tui/TrackPropertiesEditorCompletionTest.cpp) protects completion navigation and dismissal.
- [`EditorMouseTest.cpp`](../../../test/unit/tui/EditorMouseTest.cpp) protects editor mouse routing; [`MouseBindingsTest.cpp`](../../../test/unit/tui/MouseBindingsTest.cpp) protects painted shortcut bindings and clipped hit regions.

## Related documents

- [TUI interaction specification](../../spec/tui/interaction.md)
- [Predicate language reference](../query/predicate-language.md)
- [Track preset reference](../presentation/track-preset.md)
