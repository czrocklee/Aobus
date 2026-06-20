# Keyboard Shortcuts

Aobus binds keyboard accelerators to **layout action ids** (the same stable ids
exported through the action registry, e.g. `playback.playPause`). The binding is
data-driven and user-customizable: defaults ship in code, user overrides persist
in `config.yaml`, and the GTK frontend translates the result into native
accelerators at startup.

## Library / binary boundary

The action id is the neutral join key, so everything except the native
key-symbol translation lives in the platform-neutral `ao_app_uimodel` layer.

| Concern | Location | Layer |
|---|---|---|
| Neutral chord value type | `ao/uimodel/input/KeyChord` | `ao_app_uimodel` |
| Keymap (defaults, merge, conflicts, validation) | `ao/uimodel/input/KeymapModel` | `ao_app_uimodel` |
| Persistence (delta vs defaults) | `ao/uimodel/input/KeymapStore` (uses `rt::ConfigStore`) | `ao_app_uimodel` |
| Chord ⇄ GTK accelerator string | `app/GtkAccelTranslator` | `linux-gtk` |
| Application of accelerators | `app/KeymapApplicator` (`applyKeymapAccelerators`) | `linux-gtk` |
| Editor window | `app/KeyboardShortcutsWindow` | `linux-gtk` |

`ao_app_uimodel` depends on `ao_app_runtime` (never the reverse), so the keymap
model — which must validate ids against the `uimodel::layout::ActionCatalog` —
lives in `ao_app_uimodel` and merely *uses* runtime's generic `ConfigStore` as a
serialization mechanism. The runtime layer gains no knowledge of keymaps. No GDK
keysym or accelerator-string syntax appears outside `linux-gtk`, so a future
WinUI/TUI frontend reuses the model unchanged with its own translator.

`KeymapModel` is a **projection** over action ids; the `ActionRegistry` /
`ActionCatalog` remains the authoritative source for action existence and state.

## Chord representation

A `KeyChord` is zero or more modifiers plus one neutral key token. Its canonical
string form (`KeyChord::toString()`) is what gets persisted:

- modifiers `Ctrl`, `Shift`, `Alt`, `Super` (in that order), each followed by `+`;
- single ASCII letters are stored uppercase (`Ctrl+P`);
- named keys use stable spellings (`Right`, `Space`, `Enter`, `PageUp`, `F5`);
- media keys use the `Media:` prefix (`Media:Play`, `Media:Next`);
- the literal `+` key, which collides with the modifier separator, is encoded as
  a trailing `++` (`Ctrl++`) or a lone `+`; `parse` recovers it as the `+` key
  while still rejecting a dangling modifier with no key (`Ctrl+`).

`KeyChord::parse` accepts case-insensitive modifier aliases (`Control`/`Primary`
for Ctrl, `Meta`/`Cmd`/`Win` for Super).

## Default keymap

`ao::uimodel::input::defaultKeymap()` is the shipped policy (matching the
historical hard-coded accelerators):

| Action | Chords |
|---|---|
| `playback.playPause` | `Ctrl+P`, `Media:Play`, `Media:Pause` |
| `playback.stop` | `Media:Stop` |
| `playback.next` | `Ctrl+Right`, `Media:Next` |
| `playback.previous` | `Ctrl+Left`, `Media:Prev` |
| `playback.toggleShuffle` | `Ctrl+U` |
| `playback.cycleRepeat` | `Ctrl+R` |
| `workspace.revealCurrentTrack` | `Ctrl+L` |

## Configuration

User overrides persist in the `shortcuts` group of the global `config.yaml`. Only
the **delta from defaults** is written, so default changes in future versions
propagate to users who have not customized that action. Each action entry fully
replaces the default chords for that action; an empty list means *explicitly
unbound*.

```yaml
shortcuts:
  playback.playPause:
    - Ctrl+Shift+P
    - Media:Play
  playback.cycleRepeat: []   # unbind
```

On startup `applyKeymapAccelerators` loads `defaultKeymap()`, merges the persisted
overrides, translates each chord with `toGtkAccel`, and calls
`Gtk::Application::set_accels_for_action("win." + actionId, ...)`. Layout actions
are exported to the window action map, hence the `win.` prefix. Unparseable
chord strings and unmappable keys are skipped with a warning.

Because `set_accels_for_action` accumulates state on the application,
`applyKeymapAccelerators` first **reconciles**: it walks
`Gtk::Application::list_action_descriptions()` and clears any `win.*` accelerator
the current keymap no longer mentions. Without this, resetting an action that has
no shipped default — `KeymapModel::resetToDefault` erases the entry entirely —
would leave its old accelerator firing until restart. Every `win.*` accelerator is
owned by this function, so clearing the unmentioned ones is safe.

## Editor window

**Edit → Keyboard Shortcuts…** opens `KeyboardShortcutsWindow`, a standalone editor
backed by the same `KeymapModel`. It enumerates the shortcut-eligible actions from
the `ActionCatalog` — every action *except* those that require a widget anchor or
present a menu (`RequiresAnchor` / `PresentsMenu`), since a global accelerator has no
surface to anchor against — grouped by their declared category. The catalog supplies
the human-readable labels; the chords come from the effective keymap.

Editing is **live**: each add / remove / reset mutates the in-window model, re-renders
the list, then persists the delta (`AppConfig::saveKeymap`) and re-applies the
accelerators (`applyKeymapAccelerators`) immediately — no explicit Save step. New
shortcuts are captured by pressing the combination in a modal popup; the live key
press is translated by `GtkAccelTranslator::fromGtkKeyval`, which ignores standalone
modifier keys so the capture waits for a complete chord.

**Conflicts are surfaced explicitly.** When the captured chord is already held by another
action, the editor prompts the user (a modal `Gtk::AlertDialog`: *Reassign* / *Cancel*)
naming the current owner. Only on confirmation is the chord transferred away from the
previous owner; cancelling leaves both bindings untouched, so nothing is removed silently.
The model still exposes the transfer as a primitive (`bindChord`); the interactive flow
routes through `requestBind`, which performs the conflict check and gates the transfer
behind the prompt. The reassignment prompt is injectable (`setConflictConfirmer`) so the
decision logic is unit-tested without a live dialog. The window also surfaces any
pre-existing conflicts (e.g. from a hand-edited `config.yaml`) as a banner until resolved.

The editor is the GTK *view* only; all reusable logic (defaults, merge, conflicts,
`bind`/`unbind`/`resetToDefault`, delta serialization) lives in `KeymapModel` and is
unit-tested without a toolkit.

`MainWindow` owns the editor instance for its whole lifetime (a `std::unique_ptr`):
closing the window merely hides it, and reopening re-presents the same instance.
This keeps teardown deterministic and avoids deleting a window from inside its own
hide signal or leaving a callback that could outlive `MainWindow`. The transient
key-capture popup defers its own destruction to an idle callback whose connection is
cancelled in the editor's destructor, so a pending teardown never touches freed state.

## Not yet covered

- A fuller preferences surface (themes, playback, library) — the shortcuts window is
  the first standalone settings page and is intended to fold into one later.
- Context-sensitive bindings such as bare `Space` (handled today by
  `PlaybackShortcutPolicy`); these need a `Gtk::ShortcutController` tier guarded
  by `ActionCapabilities` rather than the global `set_accels_for_action` path.
