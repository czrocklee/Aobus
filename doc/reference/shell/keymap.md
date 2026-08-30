---
id: shell.keymap
type: reference
status: current
domain: application-shell
summary: Enumerates neutral chord syntax, modifier aliases, shipped action bindings, override shape, shortcut eligibility, and what each shell can carry out.
---
# Keyboard map

## Scope and version

This reference owns the exact neutral `KeyChord` string surface, shipped application defaults, TUI-local default additions, persisted override shape, and the rules deciding which bindings a shell installs.
Merge, conflict, editor, and shell-projection behavior belongs to the [keyboard shortcut specification](../../spec/shell/keyboard-shortcut.md).

The surface has no explicit schema version.

## Code boundary

Neutral chord and keymap values belong to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
Each shell owns its own translation to executable input: GTK the GDK keysyms and accel syntax, Windows the virtual-key codes and `KeyboardAccelerator`, and TUI the FTXUI terminal events its protocol can distinguish safely.
The keymap is written once for the whole application, so most of what it names is not something any one shell can run; deciding which bindings survive is that shell's own, and it is decided in plain C++ rather than at the toolkit boundary.

## Chord syntax

A canonical chord is zero or more modifiers followed by one key token:

```text
[Ctrl+][Shift+][Alt+][Super+]key
```

Canonical modifier order is `Ctrl`, `Shift`, `Alt`, `Super`.
Parsing accepts case-insensitive aliases:

| Canonical | Accepted aliases |
|---|---|
| `Ctrl` | `Ctrl`, `Control`, `Primary` |
| `Shift` | `Shift` |
| `Alt` | `Alt`, `Option` |
| `Super` | `Super`, `Meta`, `Cmd`, `Win`, `Windows` |

Single ASCII letters canonicalize to uppercase.
Digits and punctuation are stored verbatim.
Named keys use stable spellings such as `Right`, `Space`, `Enter`, `PageUp`, and `F5`.
Media keys use the `Media:` prefix, such as `Media:Play` and `Media:Next`.

The literal plus key is `+` without modifiers or a trailing doubled plus after modifiers, such as `Ctrl++`.
`Ctrl+` is invalid because it names a modifier without a key.

## Shipped application defaults

| Action id | Ordered chords |
|---|---|
| `playback.playPause` | `Ctrl+P`, `Media:Play`, `Media:Pause` |
| `playback.stop` | `Media:Stop` |
| `playback.next` | `Ctrl+Right`, `Media:Next` |
| `playback.previous` | `Ctrl+Left`, `Media:Prev` |
| `playback.toggleShuffle` | `Ctrl+U` |
| `playback.cycleRepeat` | `Ctrl+R` |
| `workspace.revealCurrentTrack` | `Ctrl+L` |
| `track.orderMoveUp` | `Alt+Up` |
| `track.orderMoveDown` | `Alt+Down` |
| `track.orderMoveToTop` | `Alt+Home` |
| `track.orderMoveToBottom` | `Alt+End` |

Other actions have no shipped global shortcut.
`Ctrl+,` is an app-scoped fixed preference accelerator and is not part of this keymap.

### TUI-local additions

The TUI starts with the shipped application defaults above, prepends its preferred terminal chords to shared actions, and adds terminal-only actions without changing `defaultKeymap()` for another shell.
The resulting additional defaults are:

| Action id | Chords prepended or added by the TUI |
|---|---|
| `tui.shell.quit` | `Q` |
| `tui.shell.toggleListChooser` | `L` |
| `tui.shell.toggleTrackDetail` | `D` |
| `tui.shell.toggleAudioQuality` | `A` |
| `tui.shell.toggleOutputDevices` | `O` |
| `tui.shell.togglePresentationChooser` | `V` |
| `tui.shell.toggleNotifications` | `N` |
| `tui.shell.showHelp` | `?` |
| `tui.shell.openCommandPalette` | `:` |
| `tui.library.openQuickFilter` | `/` |
| `tui.library.clearFilter` | `C` |
| `tui.library.reloadActiveList` | `R` |
| `tui.library.playSelection` | `Enter`, `P` |
| `tui.library.previousSection` | `{` |
| `tui.library.nextSection` | `}` |
| `tui.playback.seekBackward` | `[` |
| `tui.playback.seekForward` | `]` |
| `tui.playback.volumeDown` | `-` |
| `tui.playback.volumeUp` | `+`, `=` |
| `playback.playPause` | `Space` before the shared chords |
| `playback.stop` | `S` before the shared media chord |

`workspace.revealCurrentTrack` uses the shared `Ctrl+L` default unchanged.
The stored `tui.*` ids are compatibility surfaces; `tui.library.playSelection` deliberately differs from shared `playback.play`, because it starts a sequence from the selected row rather than resuming the current playback subject.

### Coverage per shell

| Action family | GTK | TUI | Windows |
|---|---|---|---|
| `playback.*` transport, keyboard chords | installed | Play/Pause and Stop descriptors only | installed |
| `playback.*` transport, media chords | installed | omitted | system media controls |
| `workspace.revealCurrentTrack` | installed | installed | installed |
| `track.orderMove*` | installed | no descriptor | installed |
| `tui.*` | no handler | installed when projectable | no handler |

The Windows shell installs an accelerator only when it has a handler for the action, the action does not present from an anchor, the chord is not a media key, and Windows has a key for the chord; anything else is skipped and logged rather than installed dead.

Media chords are the shell's one deliberate omission. Windows delivers the transport keys to whichever application registered for system media control, which this shell does, so those keys already run their command from anywhere - including while another application has focus. An accelerator on the same key would run it a second time whenever the window happened to be focused. `playback.stop` therefore has no Windows accelerator at all, since `Media:Stop` is its only shipped chord.

GTK installs its media chords, because there the physical key reaches the focused window and its MPRIS interface answers a different caller.
The four order shortcuts act only when the active saved List is flat and unsorted.
GTK accepts one command per physical key-down/key-up cycle, so OS auto-repeat while a key remains held does not produce repeated full-order commits.

### TUI terminal projection

The TUI builds one immutable `TuiKeymapPlan` from the effective keymap at startup.
It considers only stable action ids for which the TUI has a descriptor and handler, and the retained entries drive both root dispatch and every configurable shortcut hint.

The terminal adapter's current representation whitelist projects:

- unmodified printable ASCII characters;
- unmodified `Enter`, `Escape`, `Space`, `Tab`, `Backspace`, `Insert`, `Delete`, arrows, `Home`, `End`, `PageUp`, `PageDown`, and `F1` through `F12`;
- Shift plus an ASCII letter, and `Shift+Tab`;
- Ctrl plus any ASCII letter, plus Ctrl-modified arrows and `Ctrl+[`.

Terminal protocol aliases project to the event the terminal actually reports: `Ctrl+I` is Tab, `Ctrl+H` is Backspace, `Ctrl+J` and `Ctrl+M` are Return, `Ctrl+[` is Escape, and `Ctrl+C` is the terminal Ctrl-C event.
Alt, Super, media, Unicode, mixed-modifier, and other ambiguous or unsupported chords are omitted and diagnosed without discarding valid sibling bindings.

Representation is not installation.
The executable root plan never retains Ctrl-C, Escape or its `Ctrl+[` alias, Up/Down, Home/End, or Page Up/Down because the shell's fixed root protocol consumes those events first.
They therefore produce neither a configurable root action nor a root shortcut hint even though the adapter can represent them.

Different neutral chords can project to the same terminal event.
Descriptor order, then chord order within the effective keymap, chooses the first claimant; later claims are logged and omitted.
When a surface requests a shortcut, it also supplies any events its local protocol makes unavailable and receives that action's first retained chord executable in that request scope.
List, output, and presentation overlays therefore skip Return when selecting their toggle hint, while the notification overlay skips `x`; a later retained chord may be shown instead.
This request-scoped selection keeps dispatch and hints aligned without removing a chord that remains executable at the root or in another scope.

Fixed terminal protocol has precedence over this root plan.
Ctrl-C always requests emergency exit; active text input owns its editing, completion, submission, and cancellation keys; list and modal-overlay scopes own navigation, Return, and Escape; notification `x`, mouse sequences, and modal escape remain fixed.
The same event may therefore be configurable at the root while remaining protocol-owned in a narrower active scope.

## Override surface

GTK, TUI, and Windows read overrides from the same `shortcuts` group, each in its own application-global file: GTK in its global config, TUI in `<config>/tui.yaml`, and Windows in its desktop settings document.
The group has one shape and one schema; only the file and frontend lifecycle differ, and the [application config reference](../persistence/application-config.md) owns those document boundaries.

The `shortcuts` group is:

```text
mapping<action-id string, sequence<canonical chord string>>
```

A present action replaces its complete default list.
An empty sequence explicitly unbinds the action.
An absent action retains its current default.
Saving emits only actions whose effective ordered chords differ from their defaults.
The TUI currently has no shortcut editor and never calls `saveKeymap()` during ordinary shutdown; it only loads this group and preserves it while saving sibling application preferences.

Example:

```yaml
shortcuts:
  playback.playPause:
    - Ctrl+Shift+P
  playback.cycleRepeat: []
```

## Validation rules

- A chord requires a non-empty key token.
- A modifier segment must be recognized before the final key token.
- Equivalent parsed chords compare equal and are deduplicated within one action.
- The persistence schema requires a mapping with nonempty, nonduplicate action-id keys and sequence values whose every element is a non-null scalar string.
- A structurally valid action id need not be known to the current schema; it remains in the effective and persisted mapping but does not produce a schema-backed editor row.
- A structurally valid but unparsable chord string is skipped by semantic keymap application without discarding valid siblings.
- Shortcut-editor eligibility excludes actions with `RequiresAnchor` or `PresentsMenu`.
- A shell may skip a neutral key it cannot translate to a native accelerator without changing the stored neutral value.
- TUI ignores unknown action ids because it prepares entries from its descriptor inventory; an unknown id cannot claim a terminal event from a supported action.
- TUI projection conflicts are decided after neutral parsing because terminal aliases can collapse otherwise distinct chord strings.
- Windows additionally skips an action it registers no handler for, and any action whose schema entry declares `RequiresAnchor`, since a keystroke has no anchor to present from.
- Windows translates the letters, digits, `F1` through `F24`, the named navigation and editing keys, and the common punctuation keys. GTK additionally accepts any key GDK names.
- A punctuation character a key only produces with Shift held carries that Shift on Windows whether or not the chord spelled it. `+` and `=` are one key, so `Ctrl++` installs as Ctrl+Shift and stays distinct from `Ctrl+=`.
- Windows numbers its punctuation keys by position, and the positions this shell knows are the ones a US layout carries. On a layout that moves them, a chord naming punctuation reaches a different physical key. No shipped binding uses punctuation, so this reaches only a user's own overrides.
- Two chords that reach the same Windows key can belong to different actions once implicit modifiers are merged. The first action the map declares keeps the key; the second is reported to the log and installs nothing, rather than leaving which one runs to XAML's ordering.

## Compatibility and versioning

Canonical chord text and stable action ids are compatibility surfaces.
Changed defaults reach users only when their stored delta does not override that action.
There is no explicit migration table for renamed actions or key tokens.

## Implementation authority

- [`KeyChord.h`](../../../app/include/ao/uimodel/input/KeyChord.h) and [`KeyChord.cpp`](../../../app/uimodel/input/KeyChord.cpp) own syntax and aliases.
- [`KeymapModel.cpp`](../../../app/uimodel/input/KeymapModel.cpp) owns the default inventory.
- [`KeymapStore.cpp`](../../../app/uimodel/input/KeymapStore.cpp) owns the explicit override group schema and structural candidate policy.
- [`LayoutSchema.h`](../../../app/include/ao/uimodel/layout/component/LayoutSchema.h) owns action capability and eligibility flags.
- [`PlaybackCommand.h`](../../../app/include/ao/uimodel/playback/command/PlaybackCommand.h) owns the transport action ids every shell registers.
- [`GtkAccelTranslator.h`](../../../app/linux-gtk/app/GtkAccelTranslator.h) owns the GDK edge, and [`KeyChordAccelerator.h`](../../../app/windows-winui/include/ao/winui/input/KeyChordAccelerator.h) the Windows one.
- [`KeymapAcceleratorPlan.h`](../../../app/windows-winui/include/ao/winui/input/KeymapAcceleratorPlan.h) owns which bindings the Windows shell installs.
- [`TuiKeymap.h`](../../../app/tui/TuiKeymap.h) and [`TuiKeymap.cpp`](../../../app/tui/TuiKeymap.cpp) own TUI-only descriptors, default additions, the terminal whitelist, collision policy, and the immutable dispatch/hint plan.
- The `tui_keymap_load_only` rule in [`ArchitectureAudit.cmake`](../../../app/cmake/ArchitectureAudit.cmake) prevents the TUI from acquiring a keymap write path before it has an acknowledged editing surface.
- WinUI [`ShellBuilder.cpp`](../../../app/windows-winui/layout/ShellBuilder.cpp) registers reveal and saved-order handlers directly in the live action registry; those native component commands remain outside the Windows layout schema.

## Test authority

- [`KeyChordTest.cpp`](../../../test/unit/uimodel/input/KeyChordTest.cpp) protects syntax, canonicalization, aliases, plus, and rejection.
- [`KeymapModelTest.cpp`](../../../test/unit/uimodel/input/KeymapModelTest.cpp) protects defaults and override/delta values.
- [`KeymapStoreTest.cpp`](../../../test/unit/uimodel/input/KeymapStoreTest.cpp) protects serialized shape, dynamic action ids, malformed-candidate rejection, and invalid-chord semantic handling.
- [`GtkAccelTranslatorTest.cpp`](../../../test/unit/linux-gtk/app/GtkAccelTranslatorTest.cpp) protects the GDK edge.
- [`KeyChordAcceleratorTest.cpp`](../../../test/unit/winui/input/KeyChordAcceleratorTest.cpp) protects the Windows key table, and [`KeymapAcceleratorPlanTest.cpp`](../../../test/unit/winui/input/KeymapAcceleratorPlanTest.cpp) the skip rules plus the shipped native-only reveal and saved-order actions. Both run on every host.
- [`TuiKeymapTest.cpp`](../../../test/unit/tui/TuiKeymapTest.cpp) protects descriptor identities, shared/local default composition, projection aliases and omissions, deterministic collisions, unbinding, and one-source dispatch/hint selection.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Keyboard shortcut specification](../../spec/shell/keyboard-shortcut.md)
- [GTK layout schema and action reference](layout-schema.md)
- [Application managed-state surface](../persistence/application-config.md)
