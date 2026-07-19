---
id: persistence.application-config
type: reference
status: current
domain: persistence
summary: Enumerates Aobus managed YAML documents, registered groups, payload authorities, schemas, and version markers.
---
# Application managed-state surface

## Scope and version

This reference owns the exact registry of application-managed YAML documents and `ConfigStore` groups.
For each entry it identifies the logical document, literal top-level group, C++ payload type, explicit schema, writer, and current version marker.
It also owns the complete field surface for the small global GTK `window`, `runtime`, `session`, and `shortcuts` groups.

It does not own platform paths, store state transitions, restore/save behavior, the nested [workspace](../workspace/session-state.md) or playback schemas, presentation semantics, shell-layout node grammar, or component-state lifecycle.
Those facts belong to the linked location reference, store specification, and domain owners.

There is no shared application-config schema version.
Version authority is per payload and is listed in the registry.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places generic grouped-file mechanics in application runtime, platform-neutral payload models in runtime or UIModel, and file/path composition in frontends.
The [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) owns semantic and writer authority for every managed-state family.

The runtime [`ConfigStore`](../../../app/include/ao/rt/ConfigStore.h) provides the common top-level group container.
Runtime and UIModel payload owners define their types and schemas; GTK and TUI select concrete store files according to the [managed file locations reference](location.md).
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

| Logical document | Literal group | Payload type | Deserialize/serialize path | Current version authority | Semantic writer |
|---|---|---|---|---|---|
| Global GTK config | `window` | `ao::gtk::WindowState` | Frontend-local `WindowStateYamlSchema`. | None. | `AppConfigStore::saveWindow`. |
| Global GTK config | `runtime` | `ao::rt::AppPrefsState` | Frontend-local `AppPrefsStateYamlSchema`. | None. | `AppConfigStore::saveAppPrefs`. |
| Global GTK config | `session` | `ao::rt::AppSessionState` | Frontend-local `AppSessionStateYamlSchema`. | None. | `AppConfigStore::saveAppSession`. |
| Global GTK config | `shortcuts` | `ao::uimodel::KeymapOverrides` | UIModel `KeymapOverridesYamlSchema`. | None. | `ao::uimodel::saveKeymap` through `AppConfigStore`. |
| Injected playback-session document | `playback-session` | `ao::rt::PlaybackSessionState` | Runtime `PlaybackSessionYamlSchema`. | Required `schemaVersion`; current value `3`. | `PlaybackSessionPersistence`. |
| Runtime workspace config | `workspace` | [`ao::rt::WorkspaceSessionState`](../workspace/session-state.md) | Runtime `WorkspaceSessionYamlSchema`. | Required `presentationVersion`; current value `1`. | `WorkspaceService`. |
| GTK library presentation | `trackView.columnLayouts` | `ao::uimodel::TrackColumnLayoutDocument` converted to `TrackColumnLayoutState`. | UIModel `TrackColumnLayoutYamlSchema`. | Required `version`; current value `1`. | `GtkLayoutStateStore`. |
| GTK library presentation | `trackView.presentations` | `ao::uimodel::ListPresentationPreferenceDocument` converted to `ListPresentationPreferenceState`. | UIModel `ListPresentationPreferenceYamlSchema`. | Required `version`; current value `1`. | `GtkLayoutStateStore`. |
| Shell layout preset | `layout` | `ao::uimodel::LayoutDocument` | UIModel `LayoutDocumentYamlSchema`. | Required `version`; accepted value `1`. | Shell-layout workflow through `ShellLayoutStore`. |
| Shell component state | No group; standalone root. | `ao::uimodel::LayoutComponentStateDocument` | UIModel `LayoutComponentStateYamlSchema`. | Root `version = 1`; each entry has `stateVersion = 1`. | Layout runtime and promotion workflow through `ShellLayoutComponentStateStore`. |

The injected playback-session document is the global GTK config in GTK composition.
It is the runtime workspace config in the current TUI composition because TUI does not inject a separate playback store.

The `session` and `playback-session` groups are unrelated payloads.
`session` records application reopen/output selection, while `playback-session` records restorable listening intent paired with one library.

### Global GTK window group

The `window` group is a mapping with these explicit fields:

| Field | YAML value | C++ default | Current validation |
|---|---|---|---|
| `width` | Signed 32-bit integer. | `989` | Scalar syntax and range only. |
| `height` | Signed 32-bit integer. | `801` | Scalar syntax and range only. |
| `maximized` | Boolean. | `false` | Canonical boolean parsing. |

Missing known fields retain the value supplied as the deserialize seed; the C++ defaults in the table apply to a default-constructed seed.
Unknown fields are tolerated, duplicate fields are rejected, and a malformed present known field rejects the complete candidate.
The persistence schema does not impose positive-size or display-geometry bounds.

### Global GTK runtime-preference group

The `runtime` group is a mapping whose fields are all strings with an empty C++ default:

| Field | Stored identity |
|---|---|
| `lastOutputBackendId` | Last selected audio backend id. |
| `lastOutputProfileId` | Last selected backend profile id. |
| `lastOutputDeviceId` | Last selected output device id. |
| `lastLayoutPreset` | Last selected shell-layout preset id. |
| `lastThemePreset` | Last selected theme preset id. |

The schema accepts arbitrary strings.
Current layout and theme workflows interpret unknown or empty preset ids through their own fallback behavior; this group does not validate those catalogs.
Missing known fields retain the deserialize seed, unknown fields are tolerated, duplicate fields are rejected, and a malformed present known field rejects the complete candidate.

### Global GTK application-session group

The `session` group is a mapping whose fields are all strings with an empty C++ default:

| Field | Stored identity |
|---|---|
| `lastLibraryPath` | Last selected music-library root path. |
| `lastOutputBackendId` | Last active audio backend id. |
| `lastOutputProfileId` | Last active backend profile id. |
| `lastOutputDeviceId` | Last active output device id. |

The schema stores `lastLibraryPath` as text and applies no normalization, existence check, or platform-path validation.
It uses the same seeded-missing, unknown-field, duplicate-field, and malformed-known-field policy as the `runtime` group.

### Global GTK shortcut group

The `shortcuts` group is itself the dynamic mapping; its complete serialized shape is:

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
The schema rejects an empty action id, duplicate action id, non-sequence binding, null or non-scalar sequence element, or other malformed group structure as one failed candidate.
After structural acceptance, `KeymapModel` treats an unparseable chord string as an invalid semantic entry and continues with other usable chords.

### Delegated payload schemas

The registry fixes the group-to-type association, but these domain owners define payload fields and meaning:

| Payload | Current schema authority |
|---|---|
| `PlaybackSessionState` | [Playback session persistence specification](../../spec/playback/session-persistence.md), [state reference](../playback/session-state.md), and [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h). |
| `WorkspaceSessionDocument` / `WorkspaceSessionState` | [Workspace session state](../workspace/session-state.md). |
| `TrackColumnLayoutDocument` / `TrackColumnLayoutState` | [Persisted presentation state](../presentation/persisted-state.md) and [`TrackColumnLayoutYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h). |
| `ListPresentationPreferenceDocument` / `ListPresentationPreferenceState` | [Persisted presentation state](../presentation/persisted-state.md), [list presentation preference specification](../../spec/presentation/list-preference.md), and [`ListPresentationPreferenceYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h). |
| `LayoutDocument` | [Shell layout document](../shell/layout-document.md) and its model-specific YAML schema. |
| `LayoutComponentStateDocument` | [Shell layout component state](../shell/layout-state.md) and its model-specific YAML schema. |

## Validation rules

- Registered group names are exact and case-sensitive.
- A conforming `ConfigStore` file has a top-level mapping; each registered group is one unique keyed direct child, and duplicate group keys reject initialization.
- Unregistered top-level groups have no application consumer even though a loaded `ConfigStore` can retain them while rewriting another group.
- Every registered payload uses an explicit owner-local schema; no field, enum, id, container, or aggregate schema is inferred from its C++ type.
- The [grouped configuration store specification](../../spec/persistence/config-store.md) owns explicit schema invocation, missing-group presence results, candidate isolation, and multi-group atomic replacement, not payload deserialization policy.
- `playback-session` accepts only schema version `3`, uses explicit enum and identifier mappings, and validates the complete structural and semantic candidate.
- `workspace` accepts only presentation version `1` and validates its complete stable presentation vocabulary before view creation.
- Both GTK presentation groups accept only version `1` and validate complete candidates before replacing seeded UIModel state.
- Shell layout files require a readable `layout` group whose payload contains `version` and `root`; `templates` is optional.
- Shell component-state roots require `version`, `preset`, and `components`; each component entry requires `type`, `stateVersion`, `baselineHash`, and `state`.
- A component-state file name's preset id must equal the deserialized root `preset` value before the store returns the document.

The domain owner decides whether a syntactically deserialized identity, enum ordinal, version marker, or nested value is usable.
This registry does not convert schema membership into restore success.

## Compatibility and versioning

| Surface | Compatibility mechanism |
|---|---|
| `window`, `runtime`, and `session` | No explicit version or migration. Their schemas retain seeded values for missing known fields and tolerate unknown fields while rejecting duplicates and malformed known fields. |
| `shortcuts` | No explicit version or migration. Action ids are dynamic; the schema strictly validates the mapping/sequence/scalar structure, and keymap semantics handle unknown actions and invalid chord text. |
| `workspace` | Nested presentation vocabulary version `1`, strict deserialization, stable textual ids, and no unversioned migration. The [workspace session state reference](../workspace/session-state.md) owns remaining root compatibility limits. |
| `trackView.columnLayouts` and `trackView.presentations` | Independent payload version `1`, strict deserialization, stable text identities, and no unversioned migration. |
| `playback-session` | Explicit schema version `3`; other versions are rejected rather than migrated. |
| `layout` | Required version `1`; unsupported versions are rejected before the root or templates are interpreted. No legacy or reflected fallback is attempted. |
| Shell component state | Required file version `1` and entry version `1`; unsupported versions are rejected before version-specific payload interpretation. No legacy fallback is attempted. |

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

- [`AppConfigStore.cpp`](../../../app/linux-gtk/app/AppConfigStore.cpp), [`WindowState.h`](../../../app/linux-gtk/app/WindowState.h), and [`AppPrefsState.h`](../../../app/include/ao/rt/AppPrefsState.h) own the global GTK groups and their frontend-local schemas.
- [`KeymapStore.h`](../../../app/include/ao/uimodel/input/KeymapStore.h) and [`KeymapModel.h`](../../../app/include/ao/uimodel/input/KeymapModel.h) own the shortcut group name and mapping payload.
- [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h), [`PlaybackSessionYamlSchema.h`](../../../app/runtime/PlaybackSessionYamlSchema.h), [`PlaybackSessionYamlSchema.cpp`](../../../app/runtime/PlaybackSessionYamlSchema.cpp), and [`PlaybackSessionPersistence.cpp`](../../../app/runtime/PlaybackSessionPersistence.cpp) own the playback group, explicit schema, payload marker, and injected-store use.
- [`WorkspaceSessionYamlSchema.h`](../../../app/runtime/WorkspaceSessionYamlSchema.h), [`WorkspaceSessionYamlSchema.cpp`](../../../app/runtime/WorkspaceSessionYamlSchema.cpp), and [`WorkspaceService.cpp`](../../../app/runtime/WorkspaceService.cpp) own the workspace group and payload conversion.
- [`GtkLayoutStateStore.cpp`](../../../app/linux-gtk/app/GtkLayoutStateStore.cpp) owns the two GTK library-presentation group names.
- [`TrackColumnLayoutYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h) and [`ListPresentationPreferenceYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h) own their exact GTK presentation payloads.
- [`ShellLayoutStore.cpp`](../../../app/linux-gtk/app/ShellLayoutStore.cpp) owns the layout-preset group and file boundary.
- [`LayoutComponentState.h`](../../../app/include/ao/uimodel/layout/component/LayoutComponentState.h), [`LayoutComponentState.cpp`](../../../app/uimodel/layout/component/LayoutComponentState.cpp), and [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp) own the standalone component-state envelope and markers.
- [`app/linux-gtk/main.cpp`](../../../app/linux-gtk/main.cpp), [`AppRuntime.cpp`](../../../app/runtime/AppRuntime.cpp), and [`app/tui/App.cpp`](../../../app/tui/App.cpp) own store selection and sharing.

## Test authority

- [`AppConfigStoreTest.cpp`](../../../test/unit/linux-gtk/app/AppConfigStoreTest.cpp) protects the `window`, `runtime`, and `session` group round trips and missing-file behavior.
- [`KeymapStoreTest.cpp`](../../../test/unit/uimodel/input/KeymapStoreTest.cpp) protects the `shortcuts` group, merge, and delta-only persistence.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp) protects the exact `playback-session` field set, schema version, and store use.
- [`WorkspaceSessionTest.cpp`](../../../test/unit/runtime/WorkspaceSessionTest.cpp) and [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) protect the `workspace` group and frontend-neutral round trip.
- [`TrackColumnLayoutYamlSchemaTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnLayoutYamlSchemaTest.cpp), [`ListPresentationPreferenceYamlSchemaTest.cpp`](../../../test/unit/uimodel/library/presentation/ListPresentationPreferenceYamlSchemaTest.cpp), and [`GtkLayoutStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp) protect both per-library GTK presentation groups, version gates, and seeded-state fallback.
- [`LayoutModelTest.cpp`](../../../test/unit/uimodel/layout/document/LayoutModelTest.cpp) protects the layout payload's YAML fields and round trip; [`ShellLayoutStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutStoreTest.cpp) protects its `layout` group and per-preset file boundary.
- [`LayoutComponentStateTest.cpp`](../../../test/unit/uimodel/layout/component/LayoutComponentStateTest.cpp) protects the standalone component-state envelope, versions, and schema; [`ShellLayoutComponentStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp) protects preset matching, pruning, and the file boundary.

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
- [RFC 0032: explicit managed-state schemas](../../rfc/0032-explicit-managed-state-schemas.md), implemented across every registry entry
