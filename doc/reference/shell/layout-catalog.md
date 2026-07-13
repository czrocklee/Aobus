---
id: shell.layout-catalog
type: reference
status: current
domain: application-shell
summary: Enumerates layout descriptor shapes, registered GTK component type ids, action ids, categories, capabilities, and binding slots.
---
# Layout catalog and actions

## Scope and version

This reference owns the exact stable identities and descriptor surfaces registered by the current GTK shell.
The live component and action catalogs have no serialized catalog version; their string ids are compatibility surfaces for customized layouts and keymaps.

Component-specific property descriptors are registered beside each factory and are the executable exact authority rendered by the layout editor.
This document enumerates all registered type and action ids and the common descriptor vocabulary.

## Code boundary

Descriptor values and catalogs belong to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).
The GTK `ComponentRegistry` and `ActionRegistry` attach concrete factories and handlers without changing the stable descriptor identities.

## Component descriptor surface

Each component descriptor contains `type`, `displayName`, `category`, `props`, `layoutProps`, `minChildren`, optional `optMaxChildren`, `surfaces`, and `actionPolicy`.

Property descriptors contain `name`, `kind`, `label`, `defaultValue`, `enumValues`, optional action-binding metadata, and an optional default action id.
Property kinds are `Bool`, `Int`, `Double`, `String`, `Enum`, `StringList`, `CssClassList`, and `Size`.
Categories are `Container`, `Decorator`, `Track`, `Playback`, `Status`, `Generic`, `Application`, `Library`, and `Layout`.
Surface capabilities are `Main` and `Tooltip`.

Action-capable components can expose these standard props according to their policy:

- `primaryAction`
- `secondaryAction`
- `primaryLongPressAction`
- `secondaryLongPressAction`

## Registered component types

| Family | Type ids |
|---|---|
| Container and decorator | `absoluteCanvas`, `box`, `centerBox`, `collapsibleSplit`, `responsiveClass`, `scroll`, `separator`, `spacer`, `split`, `tabs` |
| Generic/application/library | `app.actionButton`, `app.menuBar`, `app.menuButton`, `label`, `library.listTree`, `library.openLibraryButton`, `workspace.withDetailPane` |
| Track | `track.coverArt`, `track.detailScope`, `track.detailUndoBar`, `track.fieldGrid`, `track.presentationButton`, `track.quickFilter`, `track.selectionRegion`, `track.tagEditor`, `tracks.table` |
| Playback | `playback.audioPipelinePanel`, `playback.currentArtistLabel`, `playback.currentTitleLabel`, `playback.image`, `playback.nextButton`, `playback.outputDeviceSelector`, `playback.pauseButton`, `playback.playButton`, `playback.playPauseButton`, `playback.previousButton`, `playback.qualityIndicator`, `playback.repeatButton`, `playback.seekSlider`, `playback.shuffleButton`, `playback.soulButton`, `playback.soulPlayPauseButton`, `playback.stopButton`, `playback.timeLabel`, `playback.volumeControl` |
| Status | `status.activityStatus`, `status.messageLabel`, `status.nowPlaying`, `status.playbackDetails`, `status.selectionInfo`, `status.trackCount` |

`template` is a special document node type handled by expansion and is not a registered GTK component.

## Action descriptor surface

An action descriptor contains `id`, `label`, `category`, and a capability mask.
Capability values are `RequiresAnchor`, `RequiresActiveTrack`, `RequiresFocusedView`, and `PresentsMenu`.
The live registry additionally owns one handler and optional availability provider per descriptor.

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
- Registering a duplicate component descriptor or action descriptor does not create a second stable identity.
- A component is a container when its descriptor permits at least one child or has no maximum.
- A binding is accepted only when the action exists and its capabilities fit the component slot and available activation context.
- Gio export skips an anchored or menu-presenting action unless the context provider can supply a safe anchor.
- Tooltip construction uses only components whose surface mask admits `Tooltip`; action interaction is not attached on a tooltip surface.
- Component prop kinds, defaults, enum values, child counts, and allowed surfaces are validated against the live descriptor registered beside its factory.

## Compatibility and versioning

There is no explicit catalog version or migration registry.
Removing or renaming a component id can turn an existing customized node into a visible unknown-component error.
Removing or renaming an action id can invalidate layout bindings and keymap overrides.
Such changes require a documented migration or a deliberate compatibility break with tests.

Adding a component or action requires registration, descriptor and behavior tests, editor exposure as appropriate, and an update to this inventory.

## Implementation authority

- [`LayoutComponentCatalog.h`](../../../app/include/ao/uimodel/layout/component/LayoutComponentCatalog.h) defines component descriptors and vocabulary.
- [`LayoutActionDescriptor.h`](../../../app/include/ao/uimodel/layout/action/LayoutActionDescriptor.h), [`LayoutActionCapabilities.h`](../../../app/include/ao/uimodel/layout/action/LayoutActionCapabilities.h), and [`LayoutActionCatalog.h`](../../../app/include/ao/uimodel/layout/action/LayoutActionCatalog.h) define action metadata.
- Component registrations under [`app/linux-gtk/layout/component/`](../../../app/linux-gtk/layout/component/) own per-type metadata and factories.
- [`ShellLayoutController.cpp`](../../../app/linux-gtk/app/ShellLayoutController.cpp) owns the action inventory and handlers.

## Test authority

- [`LayoutComponentCatalogTest.cpp`](../../../test/unit/uimodel/layout/component/LayoutComponentCatalogTest.cpp) and [`LayoutActionCatalogTest.cpp`](../../../test/unit/uimodel/layout/action/LayoutActionCatalogTest.cpp) protect descriptor lookup and duplicate behavior.
- GTK component tests under [`test/unit/linux-gtk/layout/components/`](../../../test/unit/linux-gtk/layout/components/) protect factory metadata and behavior.
- Action tests under [`test/unit/linux-gtk/layout/runtime/`](../../../test/unit/linux-gtk/layout/runtime/) protect registration, validation, activation, and Gio export.
- Editor descriptor tests under [`test/unit/linux-gtk/layout/editor/`](../../../test/unit/linux-gtk/layout/editor/) protect catalog-driven editing.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Shell layout lifecycle](../../spec/shell/layout-lifecycle.md)
- [Layout document reference](layout-document.md)
- [Keyboard map reference](keymap.md)
