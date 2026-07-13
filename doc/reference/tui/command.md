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
Command/alias authority is `ShellInteractionModel.cpp`.
Key/mouse dispatch authority is `EventController.cpp`.

## Surface

### Startup options

| Option | Default/meaning |
| --- | --- |
| `-l, --library <root>` | music library root; normalized absolute path |
| `--database <path>` | default `<root>/.aobus/library`; normalized absolute path |
| `--config <path>` | default `<root>/.aobus/tui-workspace.yaml`; normalized absolute path |
| `--cover-art-mode <auto|kitty|blocks|off>` | cover renderer |
| `--log-level <trace|debug|info|warn|error|critical|off>` | case-insensitive runtime log level |
| `--version` | prints `Aobus TUI <version>` and exits |

### Command prefixes

Commands accept an optional leading `/` or `:` and are case-insensitive after trimming.

| Prefix | Action |
| --- | --- |
| `filter <text>` | quick filter |
| `presentation <id>` | set track presentation |
| `preset <id>` | set track presentation |
| `view <id>` | set track presentation |

Any submitted text that is not a known prefix or exact alias becomes a quick filter.

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
| `quit`, `q` | stop and quit |

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
| `/` / `:` | enter command mode |
| `q` / `Ctrl-C` | quit |
| `Esc` | close overlay or cancel command mode |

### Command-mode keys

| Key | Action |
| --- | --- |
| printable UTF-8 | append to draft |
| `Backspace` | remove one UTF-8 code point |
| `Up`, `Down` | move completion selection |
| `Tab` | apply selected completion |
| `Return` | submit |
| `Esc` | cancel |

### Overlay-specific keys

| Overlay | Keys |
| --- | --- |
| Lists | `l` toggle, `Return` open, `Esc` close |
| Detail | `d` toggle, `Esc` close |
| Pipeline | `a` toggle, `Esc` close |
| Output | `o` toggle, `Return` select, `Esc` close |
| Views | `v` toggle, `Return` select, `Esc` close |
| Notifications | `n` toggle, `x` hide compact/local entry when eligible, `Esc` close |
| Help | `Esc` close |

### Mouse targets

| Target/gesture | Action |
| --- | --- |
| track-table wheel | move selection by three tracks |
| table scrollbar press/drag | map visual row to selected track |
| header column edge drag | resize column for current session |
| group header click | select first track in section |
| seek rail press/drag/release | preview/final seek |
| Soul button click | toggle playback |
| Soul button hover | show quality hover panel |
| library/view/status/quality/output indicators | open corresponding panel |
| list/view/output row click | select/activate according to panel |

## Validation rules

- Command prefixes match before aliases; unknown input is quick-filter text.
- Expression-like filter arguments delegate completion to the query expression completer.
- Presentation completion includes built-in and custom preset ids.
- Command mode and overlays disable workspace seek/table gestures.
- A duration-zero seek rail is inert.

## Compatibility and versioning

The TUI input surface is unversioned.
Changing a key, alias, option, or default path requires updating this reference and the relevant model/controller test.

## Examples

```text
/filter $composer == "Bach"
/view classical-works
/notifications
```

## Implementation authority

- [`Main.cpp`](../../../app/tui/Main.cpp) registers startup options.
- [`ShellInteractionModel.cpp`](../../../app/tui/ShellInteractionModel.cpp) registers prefixes and aliases.
- [`EventController.cpp`](../../../app/tui/EventController.cpp) maps keys and mouse events.

## Test authority

- [`ShellInteractionModelTest.cpp`](../../../test/unit/tui/ShellInteractionModelTest.cpp) protects commands and aliases.
- [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp) protects keyboard and mouse mappings.
- [`CommandCompletionTest.cpp`](../../../test/unit/tui/CommandCompletionTest.cpp) protects completion routing.

## Related documents

- [TUI interaction specification](../../spec/tui/interaction.md)
- [Predicate language reference](../query/predicate-language.md)
- [Track preset reference](../presentation/track-preset.md)
