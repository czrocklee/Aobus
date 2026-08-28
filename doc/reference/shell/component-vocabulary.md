---
id: shell.component-vocabulary
type: reference
status: current
domain: application-shell
summary: Enumerates the layout component types more than one shell presents, the properties whose authored value means the same thing in each, and the rule for shell-owned extensions.
---
# Shared component vocabulary

## Scope and version

This reference owns the component type ids and property definitions that belong to no single shell: the entries returned by `uimodel::sharedComponentSchemas()`.

It is part of the unversioned version 1 layout language described by the [layout document reference](layout-document.md), and carries no version of its own.
A shell's full inventory - the shared types it registers plus the types only it has - is enumerated by its own schema reference: [GTK](layout-schema.md) and [Windows](../windows/layout-schema.md).

## Code boundary

The vocabulary belongs to the **UIModel** layer in the [system architecture](../../architecture/system-overview.md), under the [application shell architecture](../../architecture/application-shell.md).

Membership is a claim about a concept both shells present, not about how either presents it.
A shared entry decides the type id, the display name, the category, the child range, the action slots, whether runtime component state is persistent, and the properties whose authored value means the same thing everywhere.
`split` is the only shared type with `persistentState`.
Everything past that stays with the shell: which widget is constructed, what its chrome looks like, and any property only its toolkit can honor.

A type id naming one shell - `windows.navigationPane`, `collapsibleSplit` - is by that admission not shared and belongs to that shell's schema.

## Surface

### Component types

`Children` is the schema's accepted child range. The two tables below are generated from the canonical schema table and byte-checked by `LayoutSchemaTest`; shell extensions are intentionally outside this block.

<!-- BEGIN GENERATED SHARED COMPONENT SCHEMA -->
| Type | Display name | Category | Children | Surfaces | Persistent state | Shared properties | Action slots | Default actions |
|---|---|---|---|---|---|---|---|---|
| `box` | Box | Containers | 0..any | main | no | `orientation`, `spacing` | none | none |
| `split` | Split Pane | Containers | 2..2 | main | yes | `orientation` | none | none |
| `label` | Label | Generic | 0..0 | main | no | `text` | none | none |
| `actionButton` | Action Button | Generic | 0..0 | main | no | `text` | primary click, primary long press | none |
| `menuButton` | Menu Button | Generic | 0..0 | main | no | `text` | none | none |
| `app.menuBar` | Menu Bar | Application | 0..0 | main | no | none | none | none |
| `track.table` | Track Table | Tracks | 0..0 | main | no | none | none | none |
| `track.quickFilter` | Quick Filter | Tracks | 0..0 | main | no | none | none | none |
| `track.presentationButton` | Presentation Button | Tracks | 0..0 | main | no | `variant` | none | none |
| `track.coverArt` | Cover Art | Tracks | 0..0 | main | no | `placeholderStyle` | none | none |
| `playback.transportButton` | Transport Button | Playback | 0..0 | main | no | `command` | none | none |
| `playback.soulButton` | Soul Button | Playback | 0..0 | main | no | `strokeWidth`, `glyphScale` | primary click, primary long press, secondary click | none |
| `playback.seekSlider` | Seek Slider | Playback | 0..0 | main | no | none | none | none |
| `playback.timeLabel` | Time Label | Playback | 0..0 | main | no | `mode` | none | none |
| `playback.volumeControl` | Volume Control | Playback | 0..0 | main | no | none | none | none |
| `playback.outputDeviceSelector` | Output Device Selector | Playback | 0..0 | main | no | none | none | none |
| `status.activity` | Activity Status | Status | 0..0 | main | no | none | none | none |
| `status.trackCount` | Track Count | Status | 0..0 | main | no | none | none | none |
| `status.selectionInfo` | Selection Info | Status | 0..0 | main | no | none | none | none |
| `status.message` | Status Message | Status | 0..0 | main | no | none | none | none |

| Component | Property | Kind | Values | Default | Action slot |
|---|---|---|---|---|---|
| `box` | `orientation` | Enum | `vertical`, `horizontal` | `vertical` | none |
| `box` | `spacing` | Int | any | `0` | none |
| `split` | `orientation` | Enum | `vertical`, `horizontal` | `vertical` | none |
| `label` | `text` | String | any | empty | none |
| `actionButton` | `text` | String | any | empty | none |
| `menuButton` | `text` | String | any | empty | none |
| `track.presentationButton` | `variant` | Enum | `default`, `title`, `compact` | `default` | none |
| `track.coverArt` | `placeholderStyle` | Enum | `monogram`, `note`, `vinyl`, `equalizer`, `soul` | `vinyl` | none |
| `playback.transportButton` | `command` | Enum | `play`, `pause`, `playPause`, `stop`, `next`, `previous`, `toggleShuffle`, `cycleRepeat` | `playPause` | none |
| `playback.soulButton` | `strokeWidth` | Double | any | `9` | none |
| `playback.soulButton` | `glyphScale` | Double | any | `1` | none |
| `playback.timeLabel` | `mode` | Enum | `combined`, `elapsed`, `duration` | `combined` | none |
<!-- END GENERATED SHARED COMPONENT SCHEMA -->

A structural or generic primitive carries no domain prefix; every type naming part of the application's subject matter does.

`command` values are the `uimodel::PlaybackCommand` ids, which are also what a `playback.*` action id carries after its category.
`mode` values are the `uimodel::PlaybackTimeMode` readings.

### Action policy

| Type | Slots the vocabulary opens |
|---|---|
| `actionButton` | primary click, primary long press |
| `playback.soulButton` | primary click, primary long press, secondary click |
| every other shared type | none |

Which action id fills a slot by default is drawn from the shell's own action inventory and is not part of the vocabulary.

### Shell extensions

A shell imports a shared entry through `LayoutSchema::addSharedComponent()` and supplies a `ComponentSchemaExtension` for shell-only properties, placement properties, action slots, and defaults.
The added name must not be one the shared schema entry already spends: reusing a shared name describes the shared concept differently rather than extending it, which is the divergence this vocabulary exists to prevent.
Registration rejects that outright.

The rule is per type, because a property name is scoped to the component that declares it. `variant` on `label` and `variant` on `status.activity` are two properties that happen to share a name, in the way an attribute means one thing on one element and another thing elsewhere; only `variant` meaning two things on `label` would be the divergence. What the table below lists are additions to types whose shared schema entry does not spend that name.

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

Windows also widens `actionButton` from the shared primary slots to secondary click, because its button binds a right-click gesture.
GTK widens `playback.soulButton` with secondary long press because GDK distinguishes it from primary hold while Windows does not.

### A shared property means one thing

A shared property is shared because its authored value means the same thing in every shell.
Two cases that once looked shared and were not, and what they became:

- **`text` is the words a reader sees.** Windows resolved it against its resource dictionary first, so `text: AppTitleValue` showed `Aobus` there and `AppTitleValue` in GTK - one property, two meanings, and a document that could not be moved between shells. Naming a localized string is now `textResourceKey`, a Windows property; it wins where authored and falls back to `text` when the dictionary does not define the key.
- **The soul's inner mark is not one concept.** GTK's `glyph` picks which of two static ornaments the soul wears. The Windows soul draws the live transport icon and can only be asked whether to draw it at all. Each shell now names its own - GTK `glyph`, Windows `showGlyph` - and the shared schema entry carries neither. `strokeWidth` and `glyphScale`, which both shells honor identically, stay shared.

A shell that can only honor part of a shared value is the signal that the value is not shared. Splitting it is the answer; quietly honoring less of it is what makes one document mean two things.

## Validation rules

- Type ids and property names are exact and case-sensitive.
- A shell imports a shared type from the canonical entry rather than restating its display name, category, child range, persistence, or shared properties.
- The shared action slots are a floor rather than an equality. Every slot the vocabulary names remains present, and a shell whose toolkit carries another gesture may widen the mask in its extension.
- A shell that widens a slot set owns proving it: schema entry agreement cannot see a capability only one shell has, so the shell's own schema test names each slot it binds.
- A shell property may not restate a shared property's name; doing so is a contract violation at registration.
- A shell may register any subset of the shared types. Omitting one is not a departure.
- Everything else a document must satisfy - unknown types, unknown properties, child counts, action binding - is decided by the [layout document reference](layout-document.md) against the shell's live schema.

## Compatibility and versioning

The vocabulary carries no version.
Renaming a shared type or property turns an existing customized node into a visible unknown-component or unknown-property error in every shell at once, so such a change is a deliberate break that updates both schemas, both preset sets, and this reference together.

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

- [`LayoutSchema.h`](../../../app/include/ao/uimodel/layout/component/LayoutSchema.h) defines the schema values and import operation.
- [`LayoutSchema.cpp`](../../../app/uimodel/layout/component/LayoutSchema.cpp) owns the canonical shared schema table.
- [`PlaybackCommand.h`](../../../app/include/ao/uimodel/playback/command/PlaybackCommand.h) owns the `command` ids.
- [`PlaybackPositionInteraction.h`](../../../app/include/ao/uimodel/playback/seek/PlaybackPositionInteraction.h) owns the `mode` readings.
- Component registrations under [`app/linux-gtk/layout/component/`](../../../app/linux-gtk/layout/component/) and [`LayoutSchema.cpp`](../../../app/windows-winui/layout/LayoutSchema.cpp) build each shell's schema from those entries.

## Test authority

- [`LayoutSchemaTest.cpp`](../../../test/unit/uimodel/layout/component/LayoutSchemaTest.cpp) protects type-id uniqueness, schema lookup, the shell-extension rule, and the generated reference block.
- [`PlaybackCommandTest.cpp`](../../../test/unit/uimodel/playback/command/PlaybackCommandTest.cpp) protects the command ids.
- [`LayoutComponentsTest.cpp`](../../../test/unit/linux-gtk/layout/components/LayoutComponentsTest.cpp) checks that GTK imports every shared entry it registers.
- [`LayoutSchemaTest.cpp`](../../../test/unit/winui/layout/LayoutSchemaTest.cpp) protects the Windows schema and runs on every host rather than only on Windows.

## Related documents

- [Shell layout document](layout-document.md)
- [GTK layout schema and actions](layout-schema.md)
- [Windows layout schema](../windows/layout-schema.md)
- [Application shell architecture](../../architecture/application-shell.md)
