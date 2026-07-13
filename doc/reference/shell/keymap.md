---
id: shell.keymap
type: reference
status: current
domain: application-shell
summary: Enumerates neutral chord syntax, modifier aliases, shipped action bindings, override shape, and shortcut eligibility.
---
# Keyboard map

## Scope and version

This reference owns the exact neutral `KeyChord` string surface, shipped default keymap, and persisted override shape.
Merge, conflict, editor, and GTK application behavior belongs to the [keyboard shortcut specification](../../spec/shell/keyboard-shortcut.md).

The surface has no explicit schema version.

## Code boundary

Neutral chord and keymap values belong to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
GTK owns native key-value translation and accelerator syntax.

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
| `Alt` | `Alt` |
| `Super` | `Super`, `Meta`, `Cmd`, `Win` |

Single ASCII letters canonicalize to uppercase.
Digits and punctuation are stored verbatim.
Named keys use stable spellings such as `Right`, `Space`, `Enter`, `PageUp`, and `F5`.
Media keys use the `Media:` prefix, such as `Media:Play` and `Media:Next`.

The literal plus key is `+` without modifiers or a trailing doubled plus after modifiers, such as `Ctrl++`.
`Ctrl+` is invalid because it names a modifier without a key.

## Default bindings

| Action id | Ordered chords |
|---|---|
| `playback.playPause` | `Ctrl+P`, `Media:Play`, `Media:Pause` |
| `playback.stop` | `Media:Stop` |
| `playback.next` | `Ctrl+Right`, `Media:Next` |
| `playback.previous` | `Ctrl+Left`, `Media:Prev` |
| `playback.toggleShuffle` | `Ctrl+U` |
| `playback.cycleRepeat` | `Ctrl+R` |
| `workspace.revealCurrentTrack` | `Ctrl+L` |

Other actions have no shipped global shortcut.
`Ctrl+,` is an app-scoped fixed preference accelerator and is not part of this keymap.

## Override surface

The global GTK `shortcuts` group is:

```text
mapping<action-id string, sequence<canonical chord string>>
```

A present action replaces its complete default list.
An empty sequence explicitly unbinds the action.
An absent action retains its current default.
Saving emits only actions whose effective ordered chords differ from their defaults.

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
- An action id may be any string at persistence decode; catalog validation reports unknown ids later.
- Shortcut-editor eligibility excludes actions with `RequiresAnchor` or `PresentsMenu`.
- GTK may skip a neutral key it cannot translate to a native accelerator without changing the stored neutral value.

## Compatibility and versioning

Canonical chord text and stable action ids are compatibility surfaces.
Changed defaults reach users only when their stored delta does not override that action.
There is no explicit migration table for renamed actions or key tokens.

## Implementation authority

- [`KeyChord.h`](../../../app/include/ao/uimodel/input/KeyChord.h) and [`KeyChord.cpp`](../../../app/uimodel/input/KeyChord.cpp) own syntax and aliases.
- [`KeymapModel.cpp`](../../../app/uimodel/input/KeymapModel.cpp) owns the default inventory.
- [`KeymapStore.cpp`](../../../app/uimodel/input/KeymapStore.cpp) owns the override group codec.
- [`LayoutActionCapabilities.h`](../../../app/include/ao/uimodel/layout/action/LayoutActionCapabilities.h) owns eligibility flags.

## Test authority

- [`KeyChordTest.cpp`](../../../test/unit/uimodel/input/KeyChordTest.cpp) protects syntax, canonicalization, aliases, plus, and rejection.
- [`KeymapModelTest.cpp`](../../../test/unit/uimodel/input/KeymapModelTest.cpp) protects defaults and override/delta values.
- [`KeymapStoreTest.cpp`](../../../test/unit/uimodel/input/KeymapStoreTest.cpp) protects serialized shape.
- [`GtkAccelTranslatorTest.cpp`](../../../test/unit/linux-gtk/app/GtkAccelTranslatorTest.cpp) protects the platform edge.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Keyboard shortcut specification](../../spec/shell/keyboard-shortcut.md)
- [Layout catalog and action reference](layout-catalog.md)
- [Application managed-state surface](../persistence/application-config.md)
