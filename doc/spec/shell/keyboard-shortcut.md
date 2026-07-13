---
id: shell.keyboard-shortcut
type: spec
status: current
domain: application-shell
summary: Defines neutral keymap merge, conflict, editing, persistence, and GTK accelerator application behavior.
---
# Keyboard shortcuts

## Scope

This specification defines current keyboard-shortcut behavior from neutral chord parsing and default/override merge through action validation, conflict handling, persistence, GTK translation, accelerator reconciliation, and live preference editing.
The [keyboard map reference](../../reference/shell/keymap.md) owns exact chord syntax, default bindings, and persisted shape.

## Code boundary

This contract spans the **UIModel** and **GTK frontend** layers in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
Neutral values and policy live under `app/include/ao/uimodel/input/` and `app/uimodel/input/`; GTK translation, application, and editing live under `app/linux-gtk/app/` and `app/linux-gtk/preferences/`.

## Terminology

- **Default keymap**: the shipped action-to-chord mapping.
- **Override**: a persisted complete replacement for one action's default chord list.
- **Effective keymap**: defaults after all overrides are applied.
- **Eligible action**: an action that neither requires an anchor nor presents a menu.
- **Conflict**: one neutral chord present on more than one effective action.

## Invariants

- Stable layout action ids are the join key across catalogs, keymaps, Gio actions, and GTK accelerators.
- Neutral chords contain no GDK or platform key-symbol value.
- Applying overrides always derives a new effective map from defaults; it does not merge into the previous effective state.
- An absent override retains defaults; a present empty override means explicitly unbound.
- Duplicate equivalent chords within one action are removed.
- The GTK adapter owns every `win.*` accelerator it applies and reconciles removed mappings.
- A global accelerator is offered only for an eligible action.

## State model

`KeymapModel` retains immutable-by-policy defaults and one mutable effective map.
The preference editor holds a working model, displays pre-existing conflicts, and invokes a change callback after confirmed mutations.
GTK application accelerator state is a platform projection of the current effective map rather than another keymap authority.

## Commands and transitions

`applyOverrides()` resets effective bindings to defaults, parses each override string, skips invalid strings with diagnostics, replaces the named action's chords, and deduplicates equivalent chords.

`bind()` adds a valid chord when not already present for that action.
`unbind()` removes one binding.
`resetToDefault()` removes the action's delta, and `resetAllToDefault()` restores the shipped map.
`toOverrides()` emits only actions whose effective bindings differ from defaults.

The preferences keyboard page enumerates eligible actions from `LayoutActionCatalog` and edits live.
When a requested chord belongs to another action, the GTK editor names the current owner and asks for Reassign or Cancel.
Reassign removes the old binding and adds the new one; cancel changes neither action.
Every accepted add, remove, reset, or reassignment persists the delta and reapplies accelerators immediately.

GTK application first clears `win.*` accelerator descriptions absent from the new mapping, then translates and applies the effective chords for each action.
This reconciliation prevents removed or reset shortcuts from remaining active until restart.

## Failure and cancellation

Invalid persisted chord strings are skipped and diagnosed; valid siblings still apply.
Neutral keys that cannot map to GTK accelerators are skipped with a warning.
Unknown action ids are reportable by the model and cannot become valid catalog-backed editor rows.

The editor never silently steals a conflicting binding.
Persistence currently uses application configuration's best-effort save wrapper, so a live accelerator update can precede proof of durable storage; the persistence architecture and RFC 0015 own that limitation.

Shortcut operations are synchronous and expose no cancellation.
The transient capture popup defers self-destruction through an idle callback whose connection is cancelled by its destructor.

## Persistence and versioning

Only delta overrides are stored in the global GTK shortcut group.
This allows a changed shipped default to reach users who did not customize that action while preserving explicit empty unbindings.
The exact group and shape belong to the keymap and application managed-state references.

Stable action ids and canonical chord strings are compatibility surfaces.
Renaming an action requires an explicit override migration or deliberate rejection of the old id.

## Frontend observations

The GTK preferences page groups actions by catalog category, shows effective chords, captures a complete non-modifier key combination, surfaces conflicts, and applies accepted changes live.
Standalone modifier presses do not complete capture.

App-scoped `Ctrl+,` and context-sensitive bindings such as bare Space are outside the current layout-action keymap.
They remain GTK-owned until a shared scoped-shortcut contract is defined.

## Implementation map

- [`KeyChord.cpp`](../../../app/uimodel/input/KeyChord.cpp), [`KeymapModel.cpp`](../../../app/uimodel/input/KeymapModel.cpp), and [`KeymapStore.cpp`](../../../app/uimodel/input/KeymapStore.cpp) own neutral policy and encoding.
- [`KeymapApplicator.cpp`](../../../app/linux-gtk/app/KeymapApplicator.cpp) and [`GtkAccelTranslator.cpp`](../../../app/linux-gtk/app/GtkAccelTranslator.cpp) own GTK projection.
- [`ShortcutEditorWidget.cpp`](../../../app/linux-gtk/preferences/ShortcutEditorWidget.cpp) owns live GTK editing and conflict confirmation.
- [`AppConfigStore.cpp`](../../../app/linux-gtk/app/AppConfigStore.cpp) owns the global group adapter.

## Test map

- [`KeyChordTest.cpp`](../../../test/unit/uimodel/input/KeyChordTest.cpp), [`KeymapModelTest.cpp`](../../../test/unit/uimodel/input/KeymapModelTest.cpp), and [`KeymapStoreTest.cpp`](../../../test/unit/uimodel/input/KeymapStoreTest.cpp) protect neutral behavior.
- [`KeymapApplicatorTest.cpp`](../../../test/unit/linux-gtk/app/KeymapApplicatorTest.cpp) protects reconciliation and GTK translation.
- [`ShortcutEditorWidgetTest.cpp`](../../../test/unit/linux-gtk/preferences/ShortcutEditorWidgetTest.cpp) protects eligibility, editing, conflict confirmation, and teardown.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Layout catalog and action reference](../../reference/shell/layout-catalog.md)
- [Keyboard map reference](../../reference/shell/keymap.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Shell layout lifecycle](layout-lifecycle.md)
