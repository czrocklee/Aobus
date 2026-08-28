---
id: reference.windows-desktop-state
type: reference
status: current
domain: application-shell
summary: Enumerates the native Windows desktop settings and theme YAML surfaces.
---
# Windows desktop state

## Scope and version

This reference exhaustively defines the Windows-owned `desktop` group in `%LOCALAPPDATA%\Aobus\windows-settings.yaml` and the complete `%LOCALAPPDATA%\Aobus\windows-theme.yaml` document for the unpackaged native Windows frontend.
The `desktop` group is version 3.
The same settings file also contains the shared version-2
`trackView.columnLayouts` group and version-1 `trackView.presentations` group
defined by the [persisted presentation-state reference](../presentation/persisted-state.md).
The theme document is strict and unversioned.

## Code boundary

The [system architecture](../../architecture/system-overview.md), [application shell architecture](../../architecture/application-shell.md), and [Windows desktop shell specification](../../spec/shell/windows-desktop.md) define ownership and behavior. The Windows-only `aobus-winui-lib` owns both schemas; the WinUI composition root owns their platform location and application.

## Surface

`windows-settings.yaml` is a grouped configuration document. Its `desktop` map requires `version` and accepts the following fields, each of which keeps the caller's seeded value when absent:

| Field | Type | Default |
|---|---|---|
| `version` | unsigned integer | `3` |
| `window.x`, `window.y` | signed integer | `80`, `80` |
| `window.width`, `window.height` | signed integer | `1280`, `800` |
| `window.maximized` | Boolean | `false` |
| `shellMode` | `modern` or `classic` | `modern` |
| `lastLibraryPath` | string | empty |
| `lastOutputBackendId` | string | empty |
| `lastOutputProfileId` | string | empty |
| `lastOutputDeviceId` | string | empty |
| `navigationPaneWidth` | finite number | `240` |
| `inspectorPaneWidth` | finite number | `320` |

The four window coordinates encode the native normal-position rectangle in
Windows workspace coordinates. `window.maximized` is applied independently.

Presentation choice and column layout are deliberately not members of `desktop`.
They use the shared per-list `trackView.presentations` and `trackView.columnLayouts` schemas so GTK and WinUI consume the same semantic state model without maintaining platform-specific field vocabularies.
Column layout includes stable field order, canonical sizing, and visibility.
Workspace owns the active view's current presentation and sort state.
`windows-theme.yaml` requires exactly three maps. `shared` requires `fontFamily`, `accent`, `windowBackground`, `surface`, `textPrimary`, `textSecondary`, `divider`, and `selection`. `modern` requires `navigationBackground`, `inspectorBackground`, and `nowPlayingBackground`. `classic` requires `chrome`, `toolbarBackground`, `treeBackground`, and `statusBackground`. `chrome` is `system` or `retro` and defaults to `system` when the file is absent.

Theme tokens control semantic font, text, accent, selection, divider, and surface
appearance only. Soul geometry, brand colors, aura, gradients, and motion are
not theme fields.

## Validation rules

All documented maps reject unknown keys. The `desktop` map requires only
`version`; every other field, including each member of `window`, is optional and
falls back to the seed the caller supplied. The theme maps require every field.
Window size must be at least 640 by 480.
Navigation width must be finite and in the inclusive range 120 through 360.
Inspector width must be finite and in the inclusive range 160 through 480.
The three output-selection fields accept arbitrary strings. Restore requires
non-empty backend and profile ids and rejects a profile known to be unsupported
by a published backend. A non-empty device id remains preferred and may be
submitted before catalog publication or while that endpoint is temporarily
unavailable. An empty device id is restorable only when the published selected
backend advertises a compatible empty-id default. WASAPI advertises concrete endpoint ids
and therefore requires a non-empty device id. Intent that cannot be submitted
leaves the Runtime default in effect without clearing the stored preference.
The shared presentation groups apply their own recursive list-id, field-id, ordering, dimension, and version validation.

`fontFamily` must be non-empty. Every color must be `#RRGGBB` or `#AARRGGBB` with hexadecimal digits. `classic.chrome` rejects values other than `system` and `retro`.

## Compatibility and versioning

Desktop settings read version 2 or 3, which are the versions this schema has written. Because every field other than
`version` is optional over the caller's seed, an older document is read in full
and the fields it predates keep their seeded values; the group is stamped with
the current version and the next checkpoint writes it back upgraded. A version
above 3 is rejected instead of read: this build cannot preserve fields it does
not know, so reading such a document would discard them at the next save. A
version below 2 is rejected as well, because no document was ever written that
way; the value marks a missing or malformed marker, and reading it under current
field semantics would dress a corrupt document up as an old one.
A rejected group leaves typed defaults in effect.
Each settings group is loaded independently, so rejection of `desktop` does not reject a valid shared presentation group and vice versa.
The theme has no compatibility envelope: adding, removing, or renaming a token requires coordinated schema, reference, and test changes.
Reload installs only a completely valid candidate.

Selecting an output row updates the in-memory exact requested backend, device,
and profile tuple even when the engine cannot currently confirm that route.
The selector performs no synchronous file write. The next ordinary Windows
settings checkpoint retains that preferred tuple instead of replacing it from
the coherent Runtime snapshot, then serializes `desktop`,
`trackView.columnLayouts`, and `trackView.presentations` into one
`saveTogether()` candidate and replaces the file atomically.
A destructive library restart destroys the parent `LibrarySession` and its settings writer before process creation.
The successor initially retains the loaded `lastLibraryPath`; an explicit requested root replaces that in-memory value and becomes eligible for `saveTogether()` only after successor activation.
That candidate inherits the latest in-memory preferred output tuple, including a selection requested after startup.
Failed successor startup therefore leaves the prior durable field unchanged.
The settings and theme files remain separate writer domains; changing or rejecting one does not mutate the other.

## Examples

```yaml
desktop:
  version: 3
  window: {x: 80, y: 80, width: 1280, height: 800, maximized: false}
  shellMode: modern
  lastLibraryPath: 'D:\Music'
  lastOutputBackendId: wasapi
  lastOutputProfileId: shared
  lastOutputDeviceId: '{0.0.0.00000000}.example'
  navigationPaneWidth: 240
  inspectorPaneWidth: 320
trackView.columnLayouts:
  version: 2
  layouts: []
trackView.presentations:
  version: 1
  preferences: []
```

```yaml
shared:
  fontFamily: Segoe UI Variable Text
  accent: '#06B6D4'
  windowBackground: '#111827'
  surface: '#1F2937'
  textPrimary: '#F9FAFB'
  textSecondary: '#9CA3AF'
  divider: '#374151'
  selection: '#334155'
modern:
  navigationBackground: '#0F172A'
  inspectorBackground: '#172033'
  nowPlayingBackground: '#0B1220'
classic:
  chrome: system
  toolbarBackground: '#F3F4F6'
  treeBackground: '#FFFFFF'
  statusBackground: '#E5E7EB'
```

## Implementation authority

- [`DesktopSettingsYamlSchema`](../../../app/windows-winui/include/ao/winui/DesktopSettingsYamlSchema.h)
- [`ThemeYamlSchema`](../../../app/windows-winui/include/ao/winui/Theme.h)
- [`LibraryStartupPlanner`](../../../app/include/ao/desktop/LibraryStartupPlanner.h),
  [`OutputSelection`](../../../app/include/ao/uimodel/playback/output/OutputSelection.h),
  [`DesktopOutputSelection`](../../../app/windows-winui/include/ao/winui/app/DesktopOutputSelection.h),
  [`SelectedRootCommit`](../../../app/windows-winui/include/ao/winui/app/SelectedRootCommit.h),
  [`LibrarySession`](../../../app/windows-winui/app/LibrarySession.cpp), and
  [`ThemeCoordinator`](../../../app/windows-winui/theme/ThemeCoordinator.cpp)

## Test authority

- [`DesktopSettingsYamlSchemaTest.cpp`](../../../test/unit/winui/DesktopSettingsYamlSchemaTest.cpp)
- [`OutputSelectionTest.cpp`](../../../test/unit/uimodel/playback/output/OutputSelectionTest.cpp)
- [`DesktopOutputSelectionTest.cpp`](../../../test/unit/winui/app/DesktopOutputSelectionTest.cpp)
- [`LibraryStartupPlannerTest.cpp`](../../../test/unit/desktop/LibraryStartupPlannerTest.cpp)
- [`SelectedRootCommitTest.cpp`](../../../test/unit/winui/app/SelectedRootCommitTest.cpp)
- [`ThemeTest.cpp`](../../../test/unit/winui/ThemeTest.cpp)

## Related documents

- [Windows desktop shell specification](../../spec/shell/windows-desktop.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Use the Windows desktop](../../user/use-windows-desktop.md)
