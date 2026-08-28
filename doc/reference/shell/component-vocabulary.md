---
id: shell.component-vocabulary
type: reference
status: current
domain: application-shell
summary: Enumerates the layout component types more than one shell presents, the properties whose authored value means the same thing in each, and the rule for shell-owned extensions.
---
# Shared component vocabulary

## Scope and version

This reference owns the component type ids and property definitions that belong to no single shell: the ones `uimodel::SharedLayoutComponentType` names, and the descriptor each of them contributes.

It is part of the unversioned version 1 layout language described by the [layout document reference](layout-document.md), and carries no version of its own.
A shell's full inventory - the shared types it registers plus the types only it has - is enumerated by its own catalog reference: [GTK](layout-catalog.md) and [Windows](../windows/layout-catalog.md).

## Code boundary

The vocabulary belongs to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).

Membership is a claim about a concept both shells present, not about how either presents it.
A shared entry decides the type id, the display name, the category, the child range, the action slots, whether runtime component state is persistent, and the properties whose authored value means the same thing everywhere.
`split` is the only shared type with `persistentState`.
Everything past that stays with the shell: which widget is constructed, what its chrome looks like, and any property only its toolkit can honor.

A type id naming one shell - `windows.navigationPane`, `collapsibleSplit` - is by that admission not shared and belongs to that shell's catalog.

## Surface

### Component types

`Children` is the descriptor's accepted child range.
`Shared properties` lists only what the vocabulary decides; each shell adds its own on top.

| Type | Display name | Category | Children | Shared properties |
|---|---|---|---|---|
| `box` | Box | Container | 0..any | `orientation`, `spacing` |
| `split` | Split Pane | Container | 2..2 | `orientation` |
| `label` | Label | Generic | 0..0 | `text` |
| `actionButton` | Action Button | Generic | 0..0 | `text` |
| `menuButton` | Menu Button | Generic | 0..0 | `text` |
| `app.menuBar` | Menu Bar | Application | 0..0 | none |
| `track.table` | Track Table | Track | 0..0 | none |
| `track.quickFilter` | Quick Filter | Track | 0..0 | none |
| `track.presentationButton` | Presentation Button | Track | 0..0 | `variant` |
| `track.coverArt` | Cover Art | Track | 0..0 | `placeholderStyle` |
| `playback.transportButton` | Transport Button | Playback | 0..0 | `command` |
| `playback.soulButton` | Soul Button | Playback | 0..0 | `strokeWidth`, `glyph`, `glyphScale` |
| `playback.seekSlider` | Seek Slider | Playback | 0..0 | none |
| `playback.timeLabel` | Time Label | Playback | 0..0 | `mode` |
| `playback.volumeControl` | Volume Control | Playback | 0..0 | none |
| `playback.outputDeviceSelector` | Output Device Selector | Playback | 0..0 | none |
| `status.activity` | Activity Status | Status | 0..0 | none |
| `status.trackCount` | Track Count | Status | 0..0 | none |
| `status.selectionInfo` | Selection Info | Status | 0..0 | none |
| `status.message` | Status Message | Status | 0..0 | none |

A structural or generic primitive carries no domain prefix; every type naming part of the application's subject matter does.

### Shared properties

| Property | Kind | Values | Default | Carried by |
|---|---|---|---|---|
| `orientation` | Enum | `vertical`, `horizontal` | `vertical` | `box`, `split` |
| `spacing` | Int | any | `0` | `box` |
| `text` | String | any | empty | `label`, `actionButton`, `menuButton` |
| `variant` | Enum | `default`, `title`, `compact` | `default` | `track.presentationButton` |
| `placeholderStyle` | Enum | `monogram`, `note`, `vinyl`, `equalizer`, `soul` | `vinyl` | `track.coverArt` |
| `command` | Enum | `play`, `pause`, `playPause`, `stop`, `next`, `previous`, `toggleShuffle`, `cycleRepeat` | `playPause` | `playback.transportButton` |
| `strokeWidth` | Double | any | the soul geometry's base stroke width | `playback.soulButton` |
| `glyph` | Enum | `none`, `sigil`, `seal` | `none` | `playback.soulButton` |
| `glyphScale` | Double | any | `1.0` | `playback.soulButton` |
| `mode` | Enum | `combined`, `elapsed`, `duration` | `combined` | `playback.timeLabel` |

`command` values are the `uimodel::PlaybackCommand` ids, which are also what a `playback.*` action id carries after its category.
`mode` values are the `uimodel::PlaybackTimeMode` readings.

### Action policy

| Type | Slots the vocabulary opens |
|---|---|
| `actionButton` | primary click, primary long press |
| `playback.soulButton` | primary click, primary long press, secondary click, secondary long press |
| every other shared type | none |

Which action id fills a slot by default is drawn from the shell's own action inventory and is not part of the vocabulary.

### Shell extensions

A shell adds properties through `withShellProperties` (or `withShellLayoutProperties` for placement properties).
The added name must not be one the shared descriptor already spends: reusing a shared name describes the shared concept differently rather than extending it, which is the divergence this vocabulary exists to prevent.
Registration rejects that outright.

The rule is per type, because a property name is scoped to the component that declares it. `variant` on `label` and `variant` on `status.activity` are two properties that happen to share a name, in the way an attribute means one thing on one element and another thing elsewhere; only `variant` meaning two things on `label` would be the divergence. What the table below lists are additions to types whose shared descriptor does not spend that name.

Extensions in the shipped shells:

| Type | GTK adds | Windows adds |
|---|---|---|
| `box` | `homogeneous` | none |
| `split` | `position`, `initialPositionPercent`, `resizeStart`, `shrinkStart`, `resizeEnd`, `shrinkEnd` | `initialPositionPercent` |
| `actionButton` | `icon`, `size`, `style` | `glyph` |
| `menuButton` | `icon`, `style` | `menuId`, `glyph` |
| `track.table` | `view`, `groupCoverPlaceholderStyle` | none |
| `track.coverArt` | `targetSize`, `forceSquare`, and the `widthRequest`, `heightRequest`, `cssClasses` layout properties | none |
| `playback.transportButton` | `showLabel`, `size` | none |
| `playback.soulButton` | `showFullLogo` | none |
| `playback.seekSlider` | none | `presentation` |
| `playback.volumeControl` | `orientation` | `presentation` |
| `status.activity` | `variant`, `idleBehavior`, `maxTextChars` | none |
| `status.trackCount`, `status.selectionInfo` | none | `variant` |

Windows also widens `actionButton` from the shared primary slots to all four, because its button binds a right-click gesture GTK's does not.

### A shared property means one thing

A shared property is shared because its authored value means the same thing in every shell.
Two cases that once looked shared and were not, and what they became:

- **`text` is the words a reader sees.** Windows resolved it against its resource dictionary first, so `text: AppTitleValue` showed `Aobus` there and `AppTitleValue` in GTK - one property, two meanings, and a document that could not be moved between shells. Naming a localized string is now `textResourceKey`, a Windows property; it wins where authored and falls back to `text` when the dictionary does not define the key.
- **The soul's inner mark is not one concept.** GTK's `glyph` picks which of two static ornaments the soul wears. The Windows soul draws the live transport icon and can only be asked whether to draw it at all. Each shell now names its own - GTK `glyph`, Windows `showGlyph` - and the shared descriptor carries neither. `strokeWidth` and `glyphScale`, which both shells honor identically, stay shared.

A shell that can only honor part of a shared value is the signal that the value is not shared. Splitting it is the answer; quietly honoring less of it is what makes one document mean two things.

## Validation rules

- Type ids and property names are exact and case-sensitive.
- A shell that registers a shared type registers the shared descriptor: the display name, category, child range, and every shared property must match. `sharedVocabularyDepartures` reports each way a catalog differs, and both shells assert it is empty.
- The shared action slots are a floor rather than an equality. Every slot the vocabulary names must be bindable in each shell that registers the type, and a shell whose toolkit carries a gesture the others lack widens the set through `withShellActionSlots`. Narrowing is rejected at registration, because a document binding a shared slot must work everywhere the type is registered.
- A shell that widens a slot set owns proving it: descriptor agreement cannot see a capability only one shell has, so the shell's own catalog test names each slot it binds.
- A shell property may not restate a shared property's name; doing so is a contract violation at registration.
- A shell may register any subset of the shared types. Omitting one is not a departure.
- Everything else a document must satisfy - unknown types, unknown properties, child counts, action binding - is decided by the [layout document reference](layout-document.md) against the shell's live catalog.

## Compatibility and versioning

The vocabulary carries no version.
Renaming a shared type or property turns an existing customized node into a visible unknown-component or unknown-property error in every shell at once, so such a change is a deliberate break that updates both catalogs, both preset sets, and this reference together.

Adding a shared type or property requires that every shell registering it can honor the value; a property only one shell can answer is a shell extension, not a shared property.

## Examples

A transport button and a time reading, authored identically for either shell:

```yaml
- type: playback.transportButton
  props:
    command: playPause
- type: playback.timeLabel
  props:
    mode: combined
```

The same soul button, with each shell's own extension alongside the shared properties:

```yaml
# GTK
- type: playback.soulButton
  props:
    glyph: sigil
    glyphScale: 1.2
    showFullLogo: false

# Windows
- type: playback.soulButton
  props:
    showGlyph: true
    glyphScale: 1.2
```

## Implementation authority

- [`SharedLayoutComponentType.h`](../../../app/include/ao/uimodel/layout/component/SharedLayoutComponentType.h) names every shared component and its shared property names.
- [`SharedLayoutComponentType.cpp`](../../../app/uimodel/layout/component/SharedLayoutComponentType.cpp) owns the shared descriptors and the departure report.
- [`PlaybackCommand.h`](../../../app/include/ao/uimodel/playback/command/PlaybackCommand.h) owns the `command` ids.
- [`PlaybackPositionInteraction.h`](../../../app/include/ao/uimodel/playback/seek/PlaybackPositionInteraction.h) owns the `mode` readings.
- Component registrations under [`app/linux-gtk/layout/component/`](../../../app/linux-gtk/layout/component/) and [`LayoutCatalog.cpp`](../../../app/windows-winui/layout/LayoutCatalog.cpp) build each shell's catalog from these descriptors.

## Test authority

- [`SharedLayoutComponentTypeTest.cpp`](../../../test/unit/uimodel/layout/component/SharedLayoutComponentTypeTest.cpp) protects type-id uniqueness, descriptor identity, the shell-extension rule, and the departure report.
- [`PlaybackCommandTest.cpp`](../../../test/unit/uimodel/playback/command/PlaybackCommandTest.cpp) protects the command ids.
- [`LayoutComponentsTest.cpp`](../../../test/unit/linux-gtk/layout/components/LayoutComponentsTest.cpp) asserts the GTK registry departs from nothing.
- [`LayoutCatalogTest.cpp`](../../../test/unit/winui/layout/LayoutCatalogTest.cpp) asserts the same of the Windows catalog, and runs on every host rather than only on Windows.

## Related documents

- [Shell layout document](layout-document.md)
- [Layout catalog and actions](layout-catalog.md)
- [Windows layout catalog](../windows/layout-catalog.md)
- [Application shell architecture](../../architecture/application-shell.md)
