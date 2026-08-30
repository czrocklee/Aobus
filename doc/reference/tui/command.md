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
Command, alias, and key-binding authority is `ShellInteractionModel.cpp`: a key that runs a command is declared once there and read by the dispatcher, the overlay handler that closes on the same key, the status bar, and the Command Palette alike.
Key and mouse dispatch is `EventController.cpp`, which also owns the one translation from a declared key's written form to a terminal event.
Keys that run no command - text-input entry, seeking, group jumps, volume - are answered where they are pressed and are named nowhere else.

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

### Command prefixes

Commands are entered through `:` and are case-insensitive after trimming.
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
| `views`, `v` | open/toggle presentation panel |
| `notifications`, `notification`, `n` | open/toggle notification center |
| `close`, `hide`, `esc` | close overlay |
| `help`, `h`, `?` | help |
| `current`, `now`, `reveal` | reveal current track |
| `clear`, `c` | clear filter |
| `reload`, `refresh`, `r` | reload active list |
| `play`, `p` | play selected track |
| `pause`, `toggle`, `space` | toggle playback |
| `stop`, `s` | stop playback |
| `quit`, `q` | request normal checkpoint-and-stop exit |

### Workspace keys

| Key | Action |
| --- | --- |
| `Up`, `Down` | previous/next track or active panel row |
| `PageUp`, `PageDown` | page selection |
| `Home`, `End` | first/last selection |
| `Return` | play selected track; in list/view/output overlay, activate row |
| `p` | play selected track |
| `Space` | toggle play/pause |
| `s` | stop |
| `[` / `]` | seek -/+ 5 seconds |
| `{` / `}` | previous/next presentation group |
| `-` / `+` / `=` | volume -/+ 5 percentage points |
| `l`, `d`, `a`, `o`, `v`, `n` | toggle corresponding overlay |
| `?` | open help |
| `Ctrl-L` | reveal current track |
| `c` | clear filter |
| `r` | reload active list |
| `/` | open an empty live Quick Filter input |
| `:` | open an empty Command Palette input |
| `q` / `Ctrl-C` | quit |
| `Esc` | close overlay or cancel active text input according to its mode |

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

Opening `/` does not copy or clear the current filter.
Confirming that untouched empty input with Return clears the filter, while closing it with Escape leaves the current filter unchanged.
After an edit, the draft also applies live following a 200-millisecond quiet interval.
The active draft replaces the bottom status bar, and its completion popup opens directly above it; the separate `:` Command Palette remains centered.

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
| Lists | `l` toggle, `Return` open, `Esc` close |
| Detail | `d` toggle, `Esc` close; every workspace key and mouse gesture below stays available while it is open |
| Pipeline | `a` toggle, `Esc` close |
| Output | `o` toggle, `Return` select, `Esc` close |
| Views | `v` toggle, `Return` select, `Esc` close |
| Notifications | `n` toggle, `x` hide compact/local entry when eligible, `Esc` close |
| Help | `Esc` close |

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
- Live Quick Filter drafts and explicit `:filter` arguments use the shared UIModel track-filter completer.
- An explicit leading query variable produces structured query suggestions; otherwise a non-empty active term produces frequency-ranked live Quick-filter value suggestions.
- Presentation completion includes built-in and custom preset ids.
- Quick-filter values come from live titles, artist, album, album artist, genre, composer, work, and tags; list names and other fields are excluded.
- Both text-input modes and modal overlays disable workspace seek/table gestures; the detail inspector does not.
- Opening or closing an overlay, entering text input, changing lists, another pointer press, or teardown cancels an unfinished column drag without saving it.
- A duration-zero seek rail is inert.

## Compatibility and versioning

The TUI input surface is unversioned.
Changing a key, alias, option, or default path requires updating this reference and the relevant model/controller test.

## Examples

```text
:filter $composer == "Bach"
:view classical-works
:notifications
```

## Implementation authority

- [`Main.cpp`](../../../app/tui/Main.cpp) registers startup options.
- [`ShellInteractionModel.cpp`](../../../app/tui/ShellInteractionModel.cpp) registers prefixes and aliases.
- [`CommandCompletion.cpp`](../../../app/tui/CommandCompletion.cpp) routes command, presentation, and shared filter completion.
- [`EventController.cpp`](../../../app/tui/EventController.cpp) maps keys and mouse events.

## Test authority

- [`ShellInteractionModelTest.cpp`](../../../test/unit/tui/ShellInteractionModelTest.cpp) protects commands and aliases.
- [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp) protects keyboard and mouse mappings.
- [`CommandCompletionTest.cpp`](../../../test/unit/tui/CommandCompletionTest.cpp) protects completion routing.

## Related documents

- [TUI interaction specification](../../spec/tui/interaction.md)
- [Predicate language reference](../query/predicate-language.md)
- [Track preset reference](../presentation/track-preset.md)
