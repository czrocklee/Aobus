---
id: persistence.application-config
type: reference
status: current
domain: persistence
summary: Enumerates Aobus managed YAML documents, registered groups, payload authorities, codecs, and version markers.
---
# Application managed-state surface

## Scope and version

This reference owns the exact registry of application-managed YAML documents and `ConfigStore` groups.
For each entry it identifies the logical document, literal top-level group, C++ payload type, codec mode, writer, and current version marker.
It also owns the complete field surface for the small global GTK `window`, `runtime`, `session`, and `shortcuts` groups.

It does not own platform paths, store state transitions, restore/save behavior, the nested [workspace](../workspace/session-state.md) or playback schemas, presentation semantics, shell-layout node grammar, or component-state lifecycle.
Those facts belong to the linked location reference, store specification, and domain owners.

There is no shared application-config schema version.
Version authority is per payload and is listed in the registry.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places generic grouped-file mechanics in application runtime, platform-neutral payload models in runtime or UIModel, and file/path composition in frontends.
The [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) owns semantic and writer authority for every managed-state family.

The runtime [`ConfigStore`](../../../app/include/ao/rt/ConfigStore.h) provides the common top-level group container.
Runtime and UIModel payload owners define their types and codecs; GTK and TUI select concrete store files according to the [managed file locations reference](location.md).
The standalone shell component-state store uses the shared YAML and atomic-file mechanisms directly and is included here so the managed-document registry is complete.

## Surface

### Logical documents

Logical names in this reference identify schemas independently of their platform paths.
The location reference owns the exact mapping from these names to Linux defaults and command-line overrides.

| Logical document | Composition | Container | Registered top-level surface |
|---|---|---|---|
| Global GTK config | One application-global GTK file. | `AppConfigStore` over one `ConfigStore`. | `window`, `runtime`, `session`, `shortcuts`, plus `playback-session` for the active library runtime. |
| Runtime workspace config | One file associated with the selected library or TUI override. | The `ConfigStore` owned by `AppRuntime`. | `workspace`; also `playback-session` when no separate playback store is injected. |
| GTK library presentation | One per-library GTK file. | `GtkLayoutStateStore` over one `ConfigStore`. | `trackView.columnLayouts` and `trackView.presentations`. |
| Shell layout preset | One user-authored file per preset id. | `ShellLayoutStore` creates a `ConfigStore` per operation. | `layout`. |
| Shell component state | One runtime-state file per preset id. | `ShellLayoutComponentStateStore` uses a standalone YAML document. | Document root; it has no `ConfigStore` group. |

The dot in `trackView.columnLayouts` and `trackView.presentations` is a literal character in one top-level YAML key.
It does not denote nested mappings.

### Group registry

| Logical document | Literal group | Payload type | Decode/encode path | Current version authority | Semantic writer |
|---|---|---|---|---|---|
| Global GTK config | `window` | `ao::gtk::WindowState` | Ordinary reflected aggregate. | None. | `AppConfigStore::saveWindow`. |
| Global GTK config | `runtime` | `ao::rt::AppPrefsState` | Ordinary reflected aggregate. | None. | `AppConfigStore::saveAppPrefs`. |
| Global GTK config | `session` | `ao::rt::AppSessionState` | Ordinary reflected aggregate. | None. | `AppConfigStore::saveAppSession`. |
| Global GTK config | `shortcuts` | `ao::uimodel::KeymapOverrides` | Ordinary string-keyed map of string vectors. | None. | `ao::uimodel::saveKeymap` through `AppConfigStore`. |
| Injected playback-session document | `playback-session` | `ao::rt::PlaybackSessionState` | `ConfigStore::loadExact` and result-returning save. | Required payload field `schemaVersion`; current value `3`. | `PlaybackSessionPersistence`. |
| Runtime workspace config | `workspace` | Private `ao::rt::detail::WorkspaceSessionDocument` converted to [`WorkspaceSessionState`](../workspace/session-state.md). | `ConfigStore::loadExact` plus runtime semantic codec. | Required `presentationVersion`; current value `1`. | `WorkspaceService`. |
| GTK library presentation | `trackView.columnLayouts` | `ao::uimodel::TrackColumnLayoutDocument` converted to `TrackColumnLayoutState`. | `ConfigStore::loadExact` plus UIModel semantic codec. | Required `version`; current value `1`. | `GtkLayoutStateStore`. |
| GTK library presentation | `trackView.presentations` | `ao::uimodel::ListPresentationPreferenceDocument` converted to `ListPresentationPreferenceState`. | `ConfigStore::loadExact` plus UIModel semantic codec. | Required `version`; current value `1`. | `GtkLayoutStateStore`. |
| Shell layout preset | `layout` | `ao::uimodel::LayoutDocument` | Model-specific YAML codec through ordinary group load/save. | Required payload field `version`; current default is `1`, but the loader does not gate its value. | Shell-layout workflow through `ShellLayoutStore`. |
| Shell component state | No group; standalone root. | `ao::uimodel::LayoutComponentStateDocument` | Model-specific direct YAML codec. | Root `version = 1`; each entry has `stateVersion = 1`. | Layout runtime and promotion workflow through `ShellLayoutComponentStateStore`. |

The injected playback-session document is the global GTK config in GTK composition.
It is the runtime workspace config in the current TUI composition because TUI does not inject a separate playback store.

The `session` and `playback-session` groups are unrelated payloads.
`session` records application reopen/output selection, while `playback-session` records restorable listening intent paired with one library.

### Global GTK window group

The `window` group is a mapping with these reflected fields:

| Field | YAML value | C++ default | Current validation |
|---|---|---|---|
| `width` | Signed 32-bit integer. | `989` | Scalar syntax and range only. |
| `height` | Signed 32-bit integer. | `801` | Scalar syntax and range only. |
| `maximized` | Boolean. | `false` | Canonical boolean parsing. |

Missing fields retain the seeded target value supplied to ordinary decode; the C++ defaults in the table apply to a default-constructed target.
The persistence codec does not impose positive-size or display-geometry bounds.

### Global GTK runtime-preference group

The `runtime` group is a mapping whose fields are all strings with an empty C++ default:

| Field | Stored identity |
|---|---|
| `lastOutputBackendId` | Last selected audio backend id. |
| `lastOutputProfileId` | Last selected backend profile id. |
| `lastOutputDeviceId` | Last selected output device id. |
| `lastLayoutPreset` | Last selected shell-layout preset id. |
| `lastThemePreset` | Last selected theme preset id. |

The codec accepts arbitrary strings.
Current layout and theme workflows interpret unknown or empty preset ids through their own fallback behavior; this group does not validate those catalogs.

### Global GTK application-session group

The `session` group is a mapping whose fields are all strings with an empty C++ default:

| Field | Stored identity |
|---|---|
| `lastLibraryPath` | Last selected music-library root path. |
| `lastOutputBackendId` | Last active audio backend id. |
| `lastOutputProfileId` | Last active backend profile id. |
| `lastOutputDeviceId` | Last active output device id. |

The codec stores `lastLibraryPath` as text and applies no normalization, existence check, or platform-path validation.

### Global GTK shortcut group

The `shortcuts` group has no reflected wrapper fields.
Its complete serialized shape is:

```text
mapping<action-id string, sequence<key-chord string>>
```

Each present action id replaces the complete shipped chord list for that action.
An empty sequence explicitly unbinds it; an absent action id retains its current shipped defaults.
Saving writes only bindings whose effective chord sequence differs from the defaults supplied to `KeymapModel`.

Action ids are plain strings in this payload.
Persistence accepts arbitrary action ids; `KeymapModel` can diagnose unknown ids against the layout action catalog, while the editor derives shortcut eligibility from that catalog.
Loading the group itself does not reject an unknown action id.
Chord values use the canonical `KeyChord::toString()` representation; the exact chord tokens, aliases, and shipped default bindings belong to the [keyboard map reference](../shell/keymap.md).

### Delegated payload schemas

The registry fixes the group-to-type association, but these domain owners define payload fields and meaning:

| Payload | Current schema authority |
|---|---|
| `PlaybackSessionState` | [Playback session persistence specification](../../spec/playback/session-persistence.md), [state reference](../playback/session-state.md), and [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h). |
| `WorkspaceSessionDocument` / `WorkspaceSessionState` | [Workspace session state](../workspace/session-state.md). |
| `TrackColumnLayoutDocument` / `TrackColumnLayoutState` | [Persisted presentation state](../presentation/persisted-state.md) and [`TrackColumnLayoutCodec.h`](../../../app/include/ao/uimodel/library/presentation/TrackColumnLayoutCodec.h). |
| `ListPresentationPreferenceDocument` / `ListPresentationPreferenceState` | [Persisted presentation state](../presentation/persisted-state.md), [list presentation preference specification](../../spec/presentation/list-preference.md), and [`ListPresentationPreferenceCodec.h`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceCodec.h). |
| `LayoutDocument` | [Shell layout document](../shell/layout-document.md) and its model-specific YAML codec. |
| `LayoutComponentStateDocument` | [Shell layout component state](../shell/layout-state.md) and its model-specific YAML codec. |

## Validation rules

- Registered group names are exact and case-sensitive.
- A conforming `ConfigStore` file has a top-level mapping; each registered group is one direct child.
- Unregistered top-level groups have no application consumer even though a loaded `ConfigStore` can retain them while rewriting another group.
- The common reflected codec writes member names exactly as declared in the payload type and writes ordinary enums as signed 32-bit numeric values.
- Ordinary group decoding, missing-field seeding, container tolerance, scalar bounds, and partial-mutation behavior belong to the [grouped configuration store specification](../../spec/persistence/config-store.md).
- `playback-session` requires exact aggregate/vector decoding before its semantic validation and accepts only schema version `3`.
- `workspace` requires exact aggregate/vector decoding, accepts only presentation version `1`, and validates its complete stable presentation vocabulary before view creation.
- Both GTK presentation groups require exact aggregate/vector decoding, accept only version `1`, and validate complete semantic candidates before replacing seeded UIModel state.
- Shell layout files require a readable `layout` group whose payload contains `version` and `root`; `templates` is optional.
- Shell component-state roots require `version`, `preset`, and `components`; each component entry requires `type`, `stateVersion`, `baselineHash`, and `state`.
- A component-state file name's preset id must equal the decoded root `preset` value before the store returns the document.

The domain owner decides whether a syntactically decoded identity, enum ordinal, version marker, or nested value is usable.
This registry does not convert schema membership into restore success.

## Compatibility and versioning

| Surface | Compatibility mechanism |
|---|---|
| `window`, `runtime`, `session`, and `shortcuts` | No explicit version or migration. Ordinary decode tolerates absent aggregate fields according to its seeded target. |
| `workspace` | Nested presentation vocabulary version `1`, strict decoding, stable textual ids, and no unversioned migration. The [workspace session state reference](../workspace/session-state.md) owns remaining root compatibility limits. |
| `trackView.columnLayouts` and `trackView.presentations` | Independent payload version `1`, strict decoding, stable text identities, and no unversioned migration. |
| `playback-session` | Explicit schema version `3`; other versions are rejected rather than migrated. |
| `layout` | A `version` field is required and emitted with default `1`, but current load does not reject another numeric version. |
| Shell component state | File version `1` and entry version `1`; mismatched versions are decoded structurally but ignored or pruned by component-state resolution. |

No file-level envelope declares which groups are present or which application version wrote them.
Moving a group to another logical document changes composition and lifecycle ownership even when its payload bytes are unchanged; a path override alone does not.

## Examples

This is a conforming global GTK config fragment for the groups whose complete field surface is owned here:

```yaml
window:
  width: 989
  height: 801
  maximized: false
runtime:
  lastOutputBackendId: pipewire
  lastOutputProfileId: default
  lastOutputDeviceId: speakers
  lastLayoutPreset: classic
  lastThemePreset: classic
session:
  lastLibraryPath: /music
  lastOutputBackendId: pipewire
  lastOutputProfileId: default
  lastOutputDeviceId: speakers
shortcuts:
  playback.playPause:
    - Ctrl+Shift+P
  playback.cycleRepeat: []
```

Mapping order is not significant.
The example intentionally omits the domain-owned `playback-session` payload.

## Implementation authority

- [`AppConfigStore.cpp`](../../../app/linux-gtk/app/AppConfigStore.cpp), [`WindowState.h`](../../../app/linux-gtk/app/WindowState.h), and [`AppPrefsState.h`](../../../app/include/ao/rt/AppPrefsState.h) own the global GTK groups and reflected fields.
- [`KeymapStore.h`](../../../app/include/ao/uimodel/input/KeymapStore.h) and [`KeymapModel.h`](../../../app/include/ao/uimodel/input/KeymapModel.h) own the shortcut group name and mapping payload.
- [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h) and [`PlaybackSessionPersistence.cpp`](../../../app/runtime/PlaybackSessionPersistence.cpp) own the playback group, payload marker, and injected-store use.
- [`WorkspaceSessionCodec.h`](../../../app/runtime/WorkspaceSessionCodec.h), [`WorkspaceSessionCodec.cpp`](../../../app/runtime/WorkspaceSessionCodec.cpp), and [`WorkspaceService.cpp`](../../../app/runtime/WorkspaceService.cpp) own the workspace group and payload conversion.
- [`GtkLayoutStateStore.cpp`](../../../app/linux-gtk/app/GtkLayoutStateStore.cpp) owns the two GTK library-presentation group names.
- [`TrackColumnLayoutCodec.h`](../../../app/include/ao/uimodel/library/presentation/TrackColumnLayoutCodec.h) and [`ListPresentationPreferenceCodec.h`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceCodec.h) own their exact GTK presentation payloads.
- [`ShellLayoutStore.cpp`](../../../app/linux-gtk/app/ShellLayoutStore.cpp) owns the layout-preset group and file boundary.
- [`LayoutComponentState.h`](../../../app/include/ao/uimodel/layout/component/LayoutComponentState.h), [`LayoutComponentState.cpp`](../../../app/uimodel/layout/component/LayoutComponentState.cpp), and [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp) own the standalone component-state envelope and markers.
- [`app/linux-gtk/main.cpp`](../../../app/linux-gtk/main.cpp), [`AppRuntime.cpp`](../../../app/runtime/AppRuntime.cpp), and [`app/tui/App.cpp`](../../../app/tui/App.cpp) own store selection and sharing.

## Test authority

- [`AppConfigStoreTest.cpp`](../../../test/unit/linux-gtk/app/AppConfigStoreTest.cpp) protects the `window`, `runtime`, and `session` group round trips and missing-file behavior.
- [`KeymapStoreTest.cpp`](../../../test/unit/uimodel/input/KeymapStoreTest.cpp) protects the `shortcuts` group, merge, and delta-only persistence.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp) protects the exact `playback-session` field set, schema version, and store use.
- [`WorkspaceSessionTest.cpp`](../../../test/unit/runtime/WorkspaceSessionTest.cpp) and [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) protect the `workspace` group and frontend-neutral round trip.
- [`TrackColumnLayoutCodecTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnLayoutCodecTest.cpp), [`ListPresentationPreferenceCodecTest.cpp`](../../../test/unit/uimodel/library/presentation/ListPresentationPreferenceCodecTest.cpp), and [`GtkLayoutStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp) protect both per-library GTK presentation groups, version gates, and seeded-state fallback.
- [`LayoutModelTest.cpp`](../../../test/unit/uimodel/layout/document/LayoutModelTest.cpp) protects the layout payload's YAML fields and round trip; [`ShellLayoutStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutStoreTest.cpp) protects its `layout` group and per-preset file boundary.
- [`LayoutComponentStateTest.cpp`](../../../test/unit/uimodel/layout/component/LayoutComponentStateTest.cpp) protects the standalone component-state envelope, versions, and codec; [`ShellLayoutComponentStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp) protects preset matching, pruning, and the file boundary.

No single test currently enumerates every registered group across all logical documents; this reference is checked against the individual authorities above.

## Related documents

- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Grouped configuration store specification](../../spec/persistence/config-store.md)
- [Atomic file replacement specification](../../spec/persistence/atomic-replacement.md)
- [Managed file locations reference](location.md)
- [Workspace session state](../workspace/session-state.md)
- [Workspace session specification](../../spec/workspace/session.md)
- [List presentation preference specification](../../spec/presentation/list-preference.md)
- [Persisted presentation state](../presentation/persisted-state.md)
- [Application shell architecture](../../architecture/application-shell.md)
- [Shell layout lifecycle specification](../../spec/shell/layout-lifecycle.md)
- [Keyboard shortcut specification](../../spec/shell/keyboard-shortcut.md)
- [RFC 0010: versioned presentation state](../../rfc/0010-versioned-presentation-state.md)
- [RFC 0015: fail-closed grouped configuration transactions](../../rfc/0015-fail-closed-config-store.md), rejected after the narrower candidate-save boundary was implemented
