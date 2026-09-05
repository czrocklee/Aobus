---
id: shell.keyboard-shortcut
type: spec
status: current
domain: application-shell
summary: Defines neutral keymap merge, conflict, persistence, desktop accelerator application, and TUI terminal projection behavior.
---
# Keyboard shortcuts

## Scope

This specification defines current keyboard-shortcut behavior from neutral chord parsing and default/override merge through action validation, conflict handling, persistence, desktop translation, TUI terminal projection, accelerator reconciliation, and live GTK preference editing.
The [keyboard map reference](../../reference/shell/keymap.md) owns exact chord syntax, default bindings, and persisted shape.

## Code boundary

This contract spans the **UIModel** and frontend layers in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
Neutral values and policy live under `app/include/ao/uimodel/input/` and `app/uimodel/input/`; GTK translation, application, and editing live under `app/linux-gtk/app/` and `app/linux-gtk/preference/`; TUI-local defaults and terminal projection live under `app/tui/`; Windows accelerator projection remains WinUI-owned.
No FTXUI event or escape-sequence value enters UIModel.

## Terminology

- **Default keymap**: the shipped action-to-chord mapping.
- **Override**: a persisted complete replacement for one action's default chord list.
- **Effective keymap**: defaults after all overrides are applied.
- **Eligible action**: an action that neither requires an anchor nor presents a menu.
- **Conflict**: one neutral chord present on more than one effective action.
- **Projected conflict**: distinct neutral chords that one frontend translates to the same executable key event.

## Invariants

- Stable layout action ids are the join key across the layout schema, keymaps, Gio actions, and GTK accelerators.
- Neutral chords contain no GDK or platform key-symbol value.
- Applying overrides always derives a new effective map from defaults; it does not merge into the previous effective state.
- An absent override retains defaults; a present empty override means explicitly unbound.
- Duplicate equivalent chords within one action are removed.
- The GTK adapter owns every `win.*` accelerator it applies and reconciles removed mappings.
- A global accelerator is offered only for an eligible action.
- The TUI prepares only actions with a stable TUI descriptor and chooses projected collisions deterministically by descriptor order, then effective chord order.
- One immutable TUI plan supplies both root dispatch and the first retained shortcut executable in the scope requesting each configurable hint.
- Terminal protocol keys retain scope precedence over configurable root actions.

## State model

`KeymapModel` retains immutable-by-policy defaults and one mutable effective map.
The preference editor holds a working model, displays pre-existing conflicts, and invokes a change callback after confirmed mutations.
GTK application accelerator state is a platform projection of the last successfully persisted effective map rather than another keymap authority.
The TUI starts from the shared defaults, adds frontend-local defaults without mutating them, applies the global override group, and prepares an immutable `TuiKeymapPlan` before constructing dispatch and rendering owners.

## Commands and transitions

`applyOverrides()` resets effective bindings to defaults, parses each override string, skips invalid strings with diagnostics, replaces the named action's chords, and deduplicates equivalent chords.

`bind()` adds a valid chord when not already present for that action.
`unbind()` removes one binding.
`resetToDefault()` removes the action's delta, and `resetAllToDefault()` restores the shipped map.
`toOverrides()` emits only actions whose effective bindings differ from defaults.

The preferences keyboard page enumerates eligible actions from `LayoutSchema` and edits live.
When a requested chord belongs to another action, the GTK editor names the current owner and asks for Reassign or Cancel.
Reassign removes the old binding and adds the new one; cancel changes neither action.
Every accepted add, remove, reset, or reassignment persists the candidate first, then replaces the live model and accelerators.
A persistence failure keeps the candidate, leaves live bindings unchanged, and shows Retry/Discard in a top banner.

GTK application first clears `win.*` accelerator descriptions absent from the new mapping, then translates and applies the effective chords for each action.
This reconciliation prevents removed or reset shortcuts from remaining active until restart.

The TUI has no editing transition in the current surface.
At startup it loads the effective map from the global application store, projects each recognized action's chords through an explicit FTXUI-event whitelist, and locally omits unsupported or later-colliding entries.
Unknown action ids are retained by neutral keymap policy but have no TUI descriptor, so they cannot consume a terminal event.
An explicit empty binding removes both the root dispatch path and every configurable hint for that action; command aliases are a separate command-language surface and remain available.
Representable Ctrl-C, Escape, Up/Down, Home/End, and Page Up/Down events are rejected by the executable root plan because fixed root protocol owns them.
For a retained root entry that becomes unavailable only in a narrower scope, the hint lookup skips that event and selects the action's next retained chord: selection overlays exclude Return from their toggle hint, and the notification overlay excludes `x`.

## Failure and cancellation

The owner-local persistence schema rejects a non-mapping group, duplicate or empty action id, non-sequence binding, or null/non-scalar sequence element as one failed candidate; the existing effective map remains unchanged.
After that structural boundary accepts the group, invalid chord strings are skipped and diagnosed while valid siblings still apply.
Neutral keys that cannot map to GTK accelerators are skipped with a warning.
Unknown action ids remain valid mapping keys but do not become schema-backed editor rows.

The TUI adapter similarly diagnoses and omits Alt, Super, media, Unicode, mixed-modifier, and otherwise unsupported terminal chords without falling back the whole keymap.
Terminal aliases such as Ctrl-I/Tab and Ctrl-M/Return are compared after projection; the earlier TUI descriptor and chord wins, and later claims are diagnosed.
Ctrl-C is representable by the adapter but never installable in the executable root plan because it belongs to the fixed graceful-exit protocol.

The editor never silently steals a conflicting binding.
The grouped store makes each requested save a fail-closed complete-document replacement.
`saveKeymap` returns that result; the GTK Keyboard page is the presentation owner and reports a failure once.
A callback without an active application window returns an error instead of silent success.
Live accelerators are not published until persistence succeeds.
Retry retries the current candidate; Discard restores the last successfully applied model.
Ordinary Preferences close protects a failed candidate; cancelling that close continues editing.
Reopening Preferences while that candidate is pending keeps the existing editor instead of rebuilding the page.
Application exit does not open a new confirmation against a disappearing parent.
A successful retry clears both the error and the unapplied state.
The store does not expose a generic commit-receipt system.

Shortcut operations are synchronous and expose no cancellation.
The transient capture popup defers self-destruction through an idle callback whose connection is cancelled by its destructor.
Saved-List order actions additionally use a platform-neutral physical-key repeat guard at the GTK dispatch edge.
That action-specific policy accepts one mutation per key-down/key-up cycle and belongs to the [saved-List order authoring specification](../presentation/list-order-authoring.md); it does not change ordinary accelerator repeat behavior.

## Persistence and versioning

When a frontend saves a keymap, it stores only delta overrides in its global shortcut group.
This allows a changed shipped default to reach users who did not customize that action while preserving explicit empty unbindings.
The exact group and shape belong to the keymap and application managed-state references.

TUI loads `shortcuts` from its global `<config>/tui.yaml` store, not from the selected library's workspace store and not from `--config`.
It has no shortcut editor and does not save the keymap on ordinary exit; this avoids normalizing untouched invalid source text without an acknowledged TUI mutation.
Saving the sibling `runtime` preference group through the same `ConfigStore` preserves the loaded shortcut group.

Stable action ids and canonical chord strings are compatibility surfaces.
Renaming an action requires an explicit override migration or deliberate rejection of the old id.

## Frontend observations

The GTK preferences page groups actions by locale-selected schema category, shows effective chords, captures a complete non-modifier key combination, surfaces localized conflict and editing copy, and applies accepted changes live.
Action ids and neutral chord strings remain the untranslated identities; localized schema labels and categories are presentation only.
Standalone modifier presses do not complete capture.

GTK app-scoped `Ctrl+,` remains outside its current layout-action keymap.
TUI bare Space is a frontend-local default for shared `playback.playPause`; protocol-owned text-input, list, overlay, notification, mouse, and escape behavior remains outside configurable root dispatch.

## Implementation map

- [`KeyChord.cpp`](../../../app/uimodel/input/KeyChord.cpp), [`KeymapModel.cpp`](../../../app/uimodel/input/KeymapModel.cpp), and [`KeymapStore.cpp`](../../../app/uimodel/input/KeymapStore.cpp) own neutral policy and the explicit override schema.
- [`KeyRepeatGuard.cpp`](../../../app/uimodel/input/KeyRepeatGuard.cpp) owns physical-key repeat suppression for mutation-sensitive actions.
- [`KeymapApplicator.cpp`](../../../app/linux-gtk/app/KeymapApplicator.cpp) and [`GtkAccelTranslator.cpp`](../../../app/linux-gtk/app/GtkAccelTranslator.cpp) own GTK projection.
- [`ShortcutEditorWidget.cpp`](../../../app/linux-gtk/preference/ShortcutEditorWidget.cpp) owns live GTK editing, conflict confirmation, failed-candidate Retry/Discard, and deferred list rebuild.
- [`AppConfigStore.cpp`](../../../app/linux-gtk/app/AppConfigStore.cpp) owns the global group adapter.
- [`TuiKeymap.cpp`](../../../app/tui/TuiKeymap.cpp) owns TUI descriptors, local defaults, terminal projection, collision resolution, and the immutable dispatch/hint plan; [`app/tui/App.cpp`](../../../app/tui/App.cpp) loads that plan from the global TUI store.

## Test map

- [`KeyChordTest.cpp`](../../../test/unit/uimodel/input/KeyChordTest.cpp), [`KeymapModelTest.cpp`](../../../test/unit/uimodel/input/KeymapModelTest.cpp), and [`KeymapStoreTest.cpp`](../../../test/unit/uimodel/input/KeymapStoreTest.cpp) protect neutral behavior.
- [`KeymapApplicatorTest.cpp`](../../../test/unit/linux-gtk/app/KeymapApplicatorTest.cpp) protects reconciliation and GTK translation.
- [`ShortcutEditorWidgetTest.cpp`](../../../test/unit/linux-gtk/preference/ShortcutEditorWidgetTest.cpp) protects eligibility, editing, conflict confirmation, failed persistence, deferred rebuild, localized chrome, and teardown.
- [`TuiKeymapTest.cpp`](../../../test/unit/tui/TuiKeymapTest.cpp), [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp), and [`RenderTest.cpp`](../../../test/unit/tui/RenderTest.cpp) protect TUI projection, fixed-scope precedence, configurable dispatch, and dynamic hints.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [GTK layout schema and action reference](../../reference/shell/layout-schema.md)
- [Keyboard map reference](../../reference/shell/keymap.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Shell layout lifecycle](layout-lifecycle.md)
- [Saved-List order authoring](../presentation/list-order-authoring.md)
