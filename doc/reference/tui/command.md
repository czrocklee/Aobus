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
Command-prefix and alias authority is `ShellInteractionModel.cpp`.
Application shortcut descriptors, TUI-local defaults, neutral-to-FTXUI translation, projected collision selection, and effective display chords belong to `TuiKeymap.cpp`.
The immutable plan built there is read by `EventController.cpp` for root dispatch and by every renderer that advertises a configurable shortcut.
`EventController.cpp` separately owns fixed text-input, list, overlay, notification, mouse, and Ctrl-C protocol, and forwards graceful exit to the App-owned `ExitController`.
`LibraryScanController.cpp` owns one restartable eager scan flight.

## Surface

### Startup options

| Option | Default/meaning |
| --- | --- |
| `-l, --library <root>` | music library root; normalized absolute path |
| `--database <path>` | default `<root>/.aobus/library`; normalized absolute path |
| `--config <path>` | workspace/playback-session file; default `<root>/.aobus/tui-workspace.yaml`; normalized absolute path; does not relocate the layout file and must not alias another TUI managed-state file |
| `--cover-art-mode <auto|kitty|blocks|off>` | cover renderer |
| `--log-level <trace|debug|info|warn|error|critical|off>` | case-insensitive runtime log level |
| `--version` | prints `Aobus TUI <version>` and exits |

Per-library column layouts and presentation preferences always use `<root>/.aobus/tui_layout.yaml`; there is no startup override for that file. Startup rejects a `--config` path that aliases this document or the global TUI application-preference document so one `ConfigStore` remains authoritative for each physical file.
Startup also exits with a diagnostic when it cannot prepare the selected workspace configuration directory.

### Shortcut overrides

At startup the TUI loads the `shortcuts` group from the application-global `<config>/tui.yaml`, using the shared application defaults plus the TUI-local defaults in the [keyboard map reference](../shell/keymap.md).
This source is independent of the selected library and `--config`.
There is no TUI shortcut editor and ordinary shutdown does not rewrite the keymap; the same global store may save the unrelated output preference while preserving the loaded shortcut group.

### Command prefixes

Commands are entered through the Command Palette (shipped root shortcut `:`) and are case-insensitive after trimming.
The parser accepts the Command Palette draft with or without its leading `:`; `/` is reserved for live Quick Filter input and is never a command prefix.

| Prefix | Action |
| --- | --- |
| `filter <text>` | quick filter |
| `presentation <id>` | set track presentation |
| `preset <id>` | set track presentation |
| `view <id>` | set track presentation |

Text that is not a known prefix or exact alias is an unknown command and does not change the filter.

### Command aliases

| Aliases | Action |
| --- | --- |
| `lists`, `l` | open/toggle list chooser |
| `detail`, `details`, `d` | open/toggle detail |
| `quality`, `audio`, `pipeline`, `a` | open/toggle quality pipeline |
| `output`, `outputs`, `device`, `devices`, `o` | open/toggle output devices |
| `views`, `p` | open/toggle presentation panel |
| `notifications`, `notification`, `n` | open/toggle notification center |
| `close`, `hide`, `esc` | close overlay |
| `help`, `h`, `?` | help |
| `current`, `now`, `reveal` | reveal current track |
| `clear`, `c` | clear filter |
| `reload`, `refresh`, `r` | reload active list |
| `scan`, `rescan` | start an eager library scan |
| `scan cancel` | request cooperative cancellation of the running scan |
| `select toggle` | mark or unmark the focused track |
| `select visual` | start a visual selection at the focused track, or confirm the running one |
| `select all` | mark every track in the current view |
| `select clear` | clear marked tracks |
| `play` | play the focused track |
| `pause`, `toggle`, `space` | toggle playback |
| `stop`, `s` | stop playback |
| `quit`, `q` | request normal checkpoint-and-stop exit |

### Workspace keys

The following table shows shipped defaults.
Except for rows marked **fixed protocol**, each action is configurable through its stable id in the [keyboard map reference](../shell/keymap.md), and every visible hint uses the effective projected shortcut.

| Default key | Action | Ownership |
| --- | --- | --- |
| `Up`, `Down` | previous/next track or active panel row | fixed protocol |
| `PageUp`, `PageDown` | page selection | fixed protocol |
| `Home`, `End` | first/last selection | fixed protocol |
| `Return` | play the focused track | configurable at root; Return is fixed activation inside supported overlays |
| `j` / `k` | next/previous track | configurable |
| `Space` | toggle play/pause | configurable |
| `s` | stop | configurable |
| `[` / `]` | seek -/+ 5 seconds | configurable |
| `{` / `}` | previous/next presentation group | configurable |
| `-` / `+` / `=` | volume -/+ 5 percentage points | configurable |
| `l`, `d`, `a`, `o`, `p`, `n` | toggle corresponding overlay | configurable |
| `?` | open help | configurable |
| `Ctrl+L` | reveal current track | configurable |
| `c` | clear filter | configurable |
| `r` | reload active list | configurable |
| `m` | mark or unmark the focused track | configurable |
| `v`, `Shift+V` | start a visual selection at the focus, or confirm the running one | configurable |
| `Shift+A` | mark every track in the current view | configurable |
| `u` | clear marked tracks | configurable |
| `/` | open an empty live Quick Filter input | configurable |
| `:` | open an empty Command Palette input | configurable |
| `q` | request normal exit | configurable |
| `Ctrl-C` | graceful exit request through the App exit gate | fixed protocol |
| `Esc` | close overlay, cancel active text input, or cancel a running visual selection according to its mode | fixed protocol |

### Quick Filter keys

| Key | Action |
| --- | --- |
| printable UTF-8 | append to draft |
| `Backspace` | remove one extended grapheme cluster |
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
Quick Filter edits are live, so Return accepts the highlighted value and Escape keeps the literal draft; Command Palette input has no live effect, so Return executes only the typed command and Escape cancels it.

### Command Palette keys

| Key | Action |
| --- | --- |
| printable UTF-8 | append to draft |
| `Backspace` | remove one extended grapheme cluster |
| `Up`, `Down` | cycle completion selection |
| `PageUp`, `PageDown` | move selection by one bounded completion page |
| `Tab` | apply selected completion and keep editing |
| `Return` | run a known command without implicitly applying the selected completion |
| `Esc` | discard the command draft and close |

### Overlay-specific keys

| Overlay | Keys |
| --- | --- |
| Lists | effective toggle (default `l`), `Return` open, `Esc` close |
| Detail | effective toggle (default `d`), `Esc` close; every workspace key and mouse gesture below stays available while it is open |
| Pipeline | effective toggle (default `a`), `Esc` close |
| Output | effective toggle (default `o`), `Return` select, `Esc` close |
| Views | effective toggle (default `p`), `Return` select, `Esc` close |
| Notifications | effective toggle (default `n`), `x` hide compact/local entry when eligible, `Esc` close |
| Help | `Esc` close; its root open shortcut is not a toggle inside the modal panel |

### Mouse targets

All track-table gestures below remain available while the detail inspector is open and are blocked by every other overlay.

| Target/gesture | Action |
| --- | --- |
| track-table wheel | move selection by three tracks |
| table scrollbar press/drag | map visual row to selected track |
| header column edge drag/release | preview a terminal-cell width, then persist the current list's canonical layout on release; interruption rolls back |
| group header click | select first track in section |
| seek rail press/drag/release | preview/final seek |
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
:notifications
```

## Implementation authority

- [`Main.cpp`](../../../app/tui/Main.cpp) registers startup options.
- [`ShellInteractionModel.cpp`](../../../app/tui/ShellInteractionModel.cpp) registers prefixes and aliases.
- [`TuiKeymap.cpp`](../../../app/tui/TuiKeymap.cpp) registers stable terminal action ids and defaults and owns executable projection plus dynamic shortcut selection.
- [`CommandCompletion.cpp`](../../../app/tui/CommandCompletion.cpp) routes command, presentation, and shared filter completion.
- [`EventController.cpp`](../../../app/tui/EventController.cpp) applies the prepared root plan after fixed scoped protocol and maps mouse events.
- [`LibraryScanController.cpp`](../../../app/tui/LibraryScanController.cpp) owns the single scan flight.
- [`ExitController.cpp`](../../../app/tui/ExitController.cpp) owns the idempotent graceful-exit gate.

## Test authority

- [`ShellInteractionModelTest.cpp`](../../../test/unit/tui/ShellInteractionModelTest.cpp) protects commands and aliases.
- [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp) protects keyboard and mouse mappings.
- [`TuiKeymapTest.cpp`](../../../test/unit/tui/TuiKeymapTest.cpp) protects defaults, supported projection, terminal aliases, collisions, unbinding, and display-chord selection.
- [`CommandCompletionTest.cpp`](../../../test/unit/tui/CommandCompletionTest.cpp) protects completion routing, including multi-word exact aliases.
- [`LibraryScanControllerTest.cpp`](../../../test/unit/tui/LibraryScanControllerTest.cpp) protects scan start, cancel, and retirement.
- [`LibraryControllerTest.cpp`](../../../test/unit/tui/LibraryControllerTest.cpp) protects mark, range, select-all, and selection publication.
- [`ExitControllerTest.cpp`](../../../test/unit/tui/ExitControllerTest.cpp) protects exit phase transitions.

## Related documents

- [TUI interaction specification](../../spec/tui/interaction.md)
- [Predicate language reference](../query/predicate-language.md)
- [Track preset reference](../presentation/track-preset.md)
