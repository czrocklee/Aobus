---
id: shell.layout-schema
type: reference
status: current
domain: application-shell
summary: Enumerates layout schema shapes, registered GTK component type ids, action ids, categories, capabilities, and binding slots.
---
# GTK layout schema and actions

## Scope and version

This reference owns the exact stable identities and schema surfaces registered by the current GTK shell.
The live `LayoutSchema` has no serialized schema version; its string ids are compatibility surfaces for customized layouts and keymaps.

Component-specific property schemas are registered beside each factory and are the executable exact authority rendered by the layout editor.
A type this shell shares with another takes its base schema from the [shared component vocabulary](component-vocabulary.md) instead, and the registration adds only GTK's own properties.
This document enumerates all registered type and action ids and the common schema vocabulary.

## Code boundary

Schema values belong to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
The GTK `ComponentRegistry` and `ActionRegistry` attach concrete factories and handlers without changing the stable schema identities.

## Component schema surface

Each `ComponentSchema` contains `id`, `displayName`, `category`, `properties`, `layoutProperties`, `minChildren`, optional `optMaxChildren`, `surfaces`, `actionSlots`, `defaultActions`, and `persistentState`.

`PropertySchema` entries contain `name`, `kind`, `label`, `defaultValue`, `enumValues`, and optional action-slot metadata. Default action ids live in the component's `defaultActions` entries.
Property kinds are `Bool`, `Int`, `Double`, `String`, `Enum`, `StringList`, and `Size`.
Categories are `Container`, `Decorator`, `Track`, `Playback`, `Status`, `Generic`, `Application`, `Library`, and `Layout`.
Surface capabilities are `Main` and `Tooltip`.

Action-capable components can expose these standard properties according to their slot mask:

- `primaryAction`
- `secondaryAction`
- `primaryLongPressAction`
- `secondaryLongPressAction`

## Registered component types

Types marked shared are registered from the [shared component vocabulary](component-vocabulary.md), which owns their display name, category, child range, action slots, and shared properties; this shell adds only what GTK alone can honor.

| Family | Type ids |
|---|---|
| Container and decorator | `absoluteCanvas`, **`box`**, `centerBox`, `collapsibleSplit`, `responsiveClass`, `scroll`, `separator`, `spacer`, **`split`**, `tabs` |
| Generic/application/library | **`actionButton`**, **`app.menuBar`**, **`label`**, **`menuButton`**, `library.listTree`, `library.openLibraryButton`, `workspace.withDetailPane` |
| Track | **`track.coverArt`**, `track.detailScope`, `track.detailUndoBar`, `track.fieldGrid`, **`track.presentationButton`**, **`track.quickFilter`**, `track.selectionRegion`, **`track.table`**, `track.tagEditor` |
| Playback | `playback.audioPipelinePanel`, `playback.currentArtistLabel`, `playback.currentTitleLabel`, `playback.image`, **`playback.outputDeviceSelector`**, `playback.qualityIndicator`, **`playback.seekSlider`**, **`playback.soulButton`**, `playback.soulPlayPauseButton`, **`playback.timeLabel`**, **`playback.transportButton`**, **`playback.volumeControl`** |
| Status | **`status.activity`**, **`status.message`**, `status.nowPlaying`, `status.playbackDetails`, **`status.selectionInfo`**, **`status.trackCount`** |

Bold ids are shared.

One transport button carries every command: `playback.transportButton` takes a `command` property rather than the shell offering a type per verb.
`playback.soulPlayPauseButton` remains GTK's own, since it is the soul drawing rather than a transport button.

`template` is a special document node type handled by expansion and is not a registered GTK component.

### Cover-art placeholder properties

The three graphical cover locations expose an enum property with the exact values `monogram`, `note`, `vinyl`, `equalizer`, and `soul`.
These properties are authored in the layout YAML and are shown by the Layout Editor; they are not application preferences.

| Component | Property | Default |
| --- | --- | --- |
| `track.table` | `groupCoverPlaceholderStyle` | `monogram` |
| `track.coverArt` | `placeholderStyle` | `vinyl` |
| `playback.image` | `placeholderStyle` | `equalizer` |

The shipped Classic/default and Modern layouts declare their intended values explicitly.
The 500-pixel cover inside the Modern playback tooltip uses `vinyl`, while its 58-pixel persistent Now Playing image uses `equalizer`.
The persistent `playback.image` remains visible and actionable with its configured placeholder, but its tooltip-surface counterpart is visible only while decoded cover art is available.
Unknown authored values fall back to the component default.

## Action schema surface

An `ActionSchema` contains `id`, `label`, `category`, and a capability mask.
Capability values are `RequiresAnchor` and `PresentsMenu`.
The live registry additionally owns one handler and optional availability provider per schema entry.
`id` is the stable binding identity. `label` and `category` are locale-selected display text; the table lists the English-root category baseline.

| Action id | Category | Capabilities |
|---|---|---|
| `playback.play` | Playback | None |
| `playback.pause` | Playback | None |
| `playback.playPause` | Playback | None |
| `playback.stop` | Playback | None |
| `playback.next` | Playback | None |
| `playback.previous` | Playback | None |
| `playback.toggleShuffle` | Playback | None |
| `playback.cycleRepeat` | Playback | None |
| `playback.showOutputDeviceSelector` | Playback | `RequiresAnchor`, `PresentsMenu` |
| `shell.showSystemMenu` | Shell | `RequiresAnchor`, `PresentsMenu` |
| `shell.showSoul` | Shell | None |
| `shell.editLayout` | Shell | None |
| `workspace.revealCurrentTrack` | Workspace | None |
| `track.presentProperties` | Tracks | None |
| `track.editTags` | Tracks | `RequiresAnchor`, `PresentsMenu` |

## Validation rules

- Component and action ids are exact and case-sensitive.
- Registering a duplicate component or action schema does not create a second stable identity.
- A component is a container when its schema permits at least one child or has no maximum.
- A binding is accepted only when the action id exists and the component exposes that action property slot; an authored empty string or `none` explicitly leaves the slot unbound and suppresses its default.
- Gio export skips an anchored or menu-presenting action unless the context provider can supply a safe anchor.
- Tooltip construction uses only components whose surface mask admits `Tooltip`; action interaction is not attached on a tooltip surface.
- GTK accepts `cssClasses` as a string or string list and rejects the Windows-only `styleKey` and `surface` fields.
- Component property kinds, defaults, enum values, child counts, and allowed surfaces are validated against the live schema registered beside its factory.

## Compatibility and versioning

There is no explicit schema version or migration registry.
Removing or renaming a component id can turn an existing customized node into a visible unknown-component error.
Removing or renaming an action id can invalidate layout bindings and keymap overrides.
Such changes require a documented migration or a deliberate compatibility break with tests.

Adding a component or action requires registration, schema and behavior tests, editor exposure as appropriate, and an update to this inventory.

## Implementation authority

- [`LayoutSchema.h`](../../../app/include/ao/uimodel/layout/component/LayoutSchema.h) defines component and action schema values, shared component entries, lookup, and action validation.
- Component registrations under [`app/linux-gtk/layout/component/`](../../../app/linux-gtk/layout/component/) own per-type metadata and factories.
- [`LayoutDialect.cpp`](../../../app/linux-gtk/layout/document/LayoutDialect.cpp) owns GTK-specific styling and authored-tooltip validation.
- [`ShellLayoutController.cpp`](../../../app/linux-gtk/app/ShellLayoutController.cpp) owns the action inventory and handlers.

## Test authority

- [`LayoutSchemaTest.cpp`](../../../test/unit/uimodel/layout/component/LayoutSchemaTest.cpp) protects schema lookup, duplicates, action-slot resolution and validation, and the generated shared-vocabulary block.
- GTK component tests under [`test/unit/linux-gtk/layout/components/`](../../../test/unit/linux-gtk/layout/components/) protect factory metadata and behavior.
- Action tests under [`test/unit/linux-gtk/layout/runtime/`](../../../test/unit/linux-gtk/layout/runtime/) protect registration, validation, activation, and Gio export.
- Editor schema tests under [`test/unit/linux-gtk/layout/editor/`](../../../test/unit/linux-gtk/layout/editor/) protect schema-driven editing.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Shell layout lifecycle](../../spec/shell/layout-lifecycle.md)
- [Layout document reference](layout-document.md)
- [Shared component vocabulary](component-vocabulary.md)
- [Keyboard map reference](keymap.md)
