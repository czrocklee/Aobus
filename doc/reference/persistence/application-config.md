---
id: persistence.application-config
---

# Application managed-state surface

## Scope and version

This reference owns the exact registry of application-managed YAML documents and `ConfigStore` groups.
For each entry it identifies the logical document, literal top-level group, C++ payload type, explicit schema, writer, and current version marker.
It also owns the complete field surface for the small global `window`, `runtime`, `session`, `preferences`, and `shortcuts` groups used by the interactive frontends, and their frontend policy when no persistent application location is available.

It does not own platform paths, generic store state transitions, domain restore/checkpoint lifecycles, the nested [workspace](../workspace/session-state.md) or playback schemas, presentation semantics, shell-layout node grammar, or component-state lifecycle.
Those facts belong to the linked location reference, store specification, and domain owners.

There is no shared application-config schema version.
Version authority is per payload and is listed in the registry.

## Code boundary

The [system architecture](../../system/overview.md) places generic grouped-file mechanics in application runtime, platform-neutral payload models in runtime or UIModel, and file/path composition in frontends.
The [persistence and managed-state architecture](../../system/persistence/README.md) owns semantic and writer authority for every managed-state family.

The runtime [`ConfigStore`](../../../app/include/ao/rt/ConfigStore.h) provides the common top-level group container.
Runtime and UIModel payload owners define their types and schemas; frontend
composition selects files according to the [managed file locations
reference](location.md). AppKit uses the ordinary workspace, playback-session,
and List-presentation preference schemas in one per-library `ConfigStore`; its native `desktop.plist` settings
are outside this YAML registry.
The standalone shell component-state store uses the shared YAML and atomic-file mechanisms directly and is included here so the managed-document registry is complete.

## Surface

### Logical documents

Logical names in this reference identify schemas independently of their platform paths.
The location reference owns platform paths and command-line overrides.

| Logical document           | Composition                                                             | Container                                                              | Registered top-level surface                                                                         |
| -------------------------- | ----------------------------------------------------------------------- | ---------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| Global GTK config          | One application-global GTK file.                                        | `AppConfigStore` over one `ConfigStore`.                               | `window`, `runtime`, `session`, `shortcuts`, plus `playback-session` for the active library runtime. |
| Global TUI config          | One application-global TUI file.                                        | One `ConfigStore` owned by the TUI composition root.                   | `runtime`, `shortcuts`, `preferences`.                                                               |
| Runtime workspace config   | One file associated with the selected library or TUI override.          | The `ConfigStore` owned by `AppRuntime`.                               | `workspace`; also `playback-session` when no separate playback store is injected.                    |
| AppKit library state       | One per-library AppKit file.                                            | The `ConfigStore` owned by `AppRuntime`, borrowed by `LibrarySession`. | `workspace`, `playback-session`, and `trackView.presentations`.                                      |
| GTK library presentation   | One per-library GTK file.                                               | `GtkLayoutStateStore` over one `ConfigStore`.                          | `trackView.columnLayouts` and `trackView.presentations`.                                             |
| TUI library presentation   | One per-library TUI file.                                               | `LayoutStateStore` over one `ConfigStore`.                             | `trackView.columnLayouts`, `trackView.presentations`, `navigation`, and `panels`.                    |
| Windows desktop settings   | One application-global WinUI file.                                      | `LibrarySession` over one `ConfigStore`.                               | `desktop` and `shortcuts`.                                                                           |
| Windows playback session   | One application-global WinUI file, kept separate from desktop settings. | The `ConfigStore` borrowed by `AppRuntime` from `LibrarySession`.      | `playback-session`.                                                                                  |
| WinUI library presentation | One per-library WinUI file.                                             | `LibrarySession` over one `ConfigStore`.                               | `trackView.columnLayouts` and `trackView.presentations`.                                             |
| Shell layout preset        | One user-authored file per preset id.                                   | `ShellLayoutStore` creates a `ConfigStore` per operation.              | `layout`.                                                                                            |
| Shell component state      | One runtime-state file per preset id.                                   | `ShellLayoutComponentStateStore` uses a standalone YAML document.      | Document root; it has no `ConfigStore` group.                                                        |

TUI validates that its runtime workspace override, per-library presentation path, and global application-preference path do not alias before it constructs their stores.

The dot in `trackView.columnLayouts` and `trackView.presentations` is a literal character in one top-level YAML key.
It does not denote nested mappings.

### Group registry

| Logical document                            | Literal group              | Payload type                                                                                                              | Deserialize/serialize path                                            | Current version authority                              | Semantic writer                                                                 |
| ------------------------------------------- | -------------------------- | ------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- | ------------------------------------------------------ | ------------------------------------------------------------------------------- |
| Global GTK config                           | `window`                   | `ao::gtk::WindowState`                                                                                                    | Frontend-local `WindowStateYamlSchema`.                               | None.                                                  | `MainWindow::saveSessionState`.                                                 |
| Global GTK config                           | `runtime`                  | `ao::rt::AppPrefsState`                                                                                                   | Runtime `AppState`, shared with every frontend that keeps this group. | Required `version`; current value `1`.                 | `AppConfigStore::saveAppPrefs`.                                                 |
| Global GTK config                           | `session`                  | `ao::rt::AppSessionState`                                                                                                 | Runtime `AppState`, shared with every frontend that keeps this group. | None.                                                  | `AppConfigStore::saveAppSession`.                                               |
| Global GTK config                           | `shortcuts`                | `ao::uimodel::KeymapOverrides`                                                                                            | UIModel `KeymapOverridesYamlSchema`.                                  | Required `version`; current value `1`.                 | `ao::uimodel::saveKeymap` through `AppConfigStore`.                             |
| Global TUI config                           | `runtime`                  | `ao::rt::AppPrefsState`                                                                                                   | Runtime `AppState`.                                                   | Required `version`; current value `1`.                 | `ao::rt::saveAppPrefs`.                                                         |
| Global TUI config                           | `shortcuts`                | `ao::uimodel::KeymapOverrides`                                                                                            | UIModel `KeymapOverridesYamlSchema`.                                  | Required `version`; current value `1`.                 | TUI Settings through `ao::uimodel::saveKeymap` and the App-owned `ConfigStore`. |
| Global TUI config                           | `preferences`              | `ao::tui::Preferences`                                                                                                    | TUI-local `PreferencesSchema`.                                        | Required `version`; current value `1`.                 | TUI Settings through the App-owned `ConfigStore`.                               |
| Windows desktop settings                    | `shortcuts`                | `ao::uimodel::KeymapOverrides`                                                                                            | UIModel `KeymapOverridesYamlSchema`.                                  | Required `version`; current value `1`.                 | None; WinUI loads hand-authored overrides.                                      |
| Frontend-selected playback-session document | `playback-session`         | `ao::rt::PlaybackSessionState`                                                                                            | Runtime `PlaybackSessionYamlSchema`.                                  | Required `schemaVersion`; current value `1`.           | `PlaybackSessionPersistence`.                                                   |
| Runtime workspace config                    | `workspace`                | [`ao::rt::WorkspaceSessionState`](../workspace/session-state.md)                                                          | Runtime `WorkspaceSessionYamlSchema`.                                 | Required `presentationVersion`; current value `3`.     | `WorkspaceService`.                                                             |
| AppKit library state                        | `trackView.presentations`  | `ao::uimodel::ListPresentationPreferenceDocument` converted to `ListPresentations::Snapshot`.                             | UIModel `ListPresentationPreferenceYamlSchema`.                       | Required `version`; current value `1`.                 | AppKit `LibrarySession`.                                                        |
| GTK library presentation                    | `trackView.columnLayouts`  | `ao::uimodel::TrackColumnLayoutDocument` converted to `TrackColumnLayouts::Snapshot`.                                     | UIModel `TrackColumnLayoutYamlSchema`.                                | Required `version`; current value `1`.                 | `GtkLayoutStateStore`.                                                          |
| GTK library presentation                    | `trackView.presentations`  | `ao::uimodel::ListPresentationPreferenceDocument` converted to `ListPresentations::Snapshot`.                             | UIModel `ListPresentationPreferenceYamlSchema`.                       | Required `version`; current value `1`.                 | `GtkLayoutStateStore`.                                                          |
| TUI library presentation                    | `trackView.columnLayouts`  | `ao::uimodel::TrackColumnLayoutDocument` converted to `TrackColumnLayouts::Snapshot`; positive widths are terminal cells. | UIModel `TrackColumnLayoutYamlSchema`.                                | Required `version`; current value `1`.                 | `LayoutStateStore`.                                                             |
| TUI library presentation                    | `trackView.presentations`  | `ao::uimodel::ListPresentationPreferenceDocument` converted to `ListPresentations::Snapshot`.                             | UIModel `ListPresentationPreferenceYamlSchema`.                       | Required `version`; current value `1`.                 | `LayoutStateStore`.                                                             |
| TUI library presentation                    | `navigation`               | Boolean `enabled`.                                                                                                        | TUI-local `NavigationSchema` in `LayoutStateStore.cpp`.               | Required `version`; current value `1`.                 | `LayoutStateStore`.                                                             |
| TUI library presentation                    | `panels`                   | `ao::tui::PanelWidths`.                                                                                                   | TUI-local `PanelWidthsSchema` in `LayoutStateStore.cpp`.              | Required `version`; current value `1`.                 | `LayoutStateStore`.                                                             |
| Windows desktop settings                    | `desktop`                  | `ao::winui::DesktopSettings`.                                                                                             | WinUI frontend `DesktopSettingsYamlSchema`.                           | Required `version`; current value `1`.                 | WinUI `LibrarySession`.                                                         |
| WinUI library presentation                  | `trackView.columnLayouts`  | `ao::uimodel::TrackColumnLayoutDocument` converted to `TrackColumnLayouts::Snapshot`.                                     | UIModel `TrackColumnLayoutYamlSchema`.                                | Required `version`; current value `1`.                 | WinUI `LibrarySession`.                                                         |
| WinUI library presentation                  | `trackView.presentations`  | `ao::uimodel::ListPresentationPreferenceDocument` converted to `ListPresentations::Snapshot`.                             | UIModel `ListPresentationPreferenceYamlSchema`.                       | Required `version`; current value `1`.                 | WinUI `LibrarySession`.                                                         |
| Shell layout preset                         | `layout`                   | `ao::uimodel::LayoutDocument`                                                                                             | UIModel `LayoutDocumentYamlSchema`.                                   | Required `version`; accepted value `1`.                | Shell-layout workflow through `ShellLayoutStore`.                               |
| Shell component state                       | No group; standalone root. | `ao::uimodel::LayoutComponentStateDocument`                                                                               | UIModel `LayoutComponentStateYamlSchema`.                             | Root `version = 1`; each entry has `stateVersion = 1`. | Layout runtime and promotion workflow through `ShellLayoutComponentStateStore`. |

The playback-session document is the global GTK config in GTK composition.
It is the runtime workspace config in the current TUI composition because TUI does not inject a separate playback store.
WinUI injects the separate global `windows-playback.yaml` store so playback retirement and write sealing do not rewrite desktop settings.

The `session` and `playback-session` groups are unrelated payloads.
`session` records application reopen state and the last engine-confirmed output route, while `playback-session` records restorable listening intent paired with one library.

### Global GTK window group

The `window` group is a mapping with these explicit fields:

| Field       | YAML value             | C++ default | Current validation            |
| ----------- | ---------------------- | ----------- | ----------------------------- |
| `width`     | Signed 32-bit integer. | `989`       | Scalar syntax and range only. |
| `height`    | Signed 32-bit integer. | `801`       | Scalar syntax and range only. |
| `maximized` | Boolean.               | `false`     | Canonical boolean parsing.    |

Missing known fields retain the value supplied as the deserialize seed; the C++ defaults in the table apply to a default-constructed seed.
Unknown fields are tolerated, duplicate fields are rejected, and a malformed present known field rejects the complete candidate.
The persistence schema does not impose positive-size or display-geometry bounds.
`width` and `height` retain the last positive dimensions observed while the window was not maximized; a maximized checkpoint updates only `maximized` and preserves those normal dimensions for the next restore.

### Global runtime-preference group

The GTK and TUI `runtime` group is a mapping with a required `version`, which must be `1` and is read before every other field:

| Field                      | Stored value                                                 |
| -------------------------- | ------------------------------------------------------------ |
| `lastOutputBackendId`      | Preferred audio backend id requested through the selector.   |
| `lastOutputProfileId`      | Preferred backend profile id requested through the selector. |
| `lastOutputDeviceId`       | Preferred output device id requested through the selector.   |
| `lastLayoutPreset`         | Last selected shell-layout preset id.                        |
| `lastThemePreset`          | Last selected theme preset id.                               |
| `allowSystemOutputReroute` | Optional boolean for a named PipeWire output preference.     |

The id fields accept arbitrary strings and have empty C++ defaults.
Current layout and theme workflows interpret unknown or empty preset ids through their own fallback behavior; this group does not validate those catalogs.
Missing ids retain the deserialize seed, unknown fields are tolerated, and a duplicate field or a malformed present id rejects the complete candidate.

Restore requires non-empty backend and profile ids. With a non-empty device id, the preference is binding even when its backend, device or profile is unavailable: the application must not select a substitute. This does not grant permission to open an unsupported profile. A missing catalog entry and a device that refuses to open have different [playback outcomes](../../system/playback/application-commit.md#snapshot-borrowing-and-output-intent).

An empty device id requests a provider-defined default, not the first enumerated device. Restore normally requires the catalog to advertise a compatible empty-id default. A preferred PipeWire shared default can be requested before discovery; WASAPI and Core Audio publish concrete endpoint ids even for their current defaults.

GTK and TUI save a named PipeWire device as `node:<node.name>`. A bare numeric id from an older file is not matched against live object numbers; reselect the output to save its name.

`allowSystemOutputReroute` has these read and write rules:

- Unquoted `true` grants permission only if the same group explicitly stores `pipewire`, a `node:` id whose name is non-empty and contains no NUL, and `shared` or `exclusive`. PipeWire may then route to another sink at startup or during playback without changing the preferred name, backend or profile.
- Missing, `false`, quoted or malformed permission values grant no permission, regardless of the seed. A malformed permission value does not reject an otherwise valid route. A missing or rejected group grants no permission either.
- Saving writes `true` or `false` for eligible routes and omits the field for other routes. The separate TUI `preferences` group and Windows and macOS settings do not store this field.

### Global GTK application-session group

The `session` group is a mapping whose fields are all strings with an empty C++ default:

| Field                 | Stored identity                        |
| --------------------- | -------------------------------------- |
| `lastLibraryPath`     | Last selected music-library root path. |
| `lastOutputBackendId` | Last active audio backend id.          |
| `lastOutputProfileId` | Last active backend profile id.        |
| `lastOutputDeviceId`  | Last active output device id.          |

The schema stores `lastLibraryPath` as text and applies no normalization, existence check, or platform-path validation.
It has no `version`. Missing fields retain the deserialize seed, unknown fields are tolerated, and duplicate or malformed known fields reject the complete candidate.

GTK considers the last-active tuple only when it has no usable preference. A complete named preference, or a preferred PipeWire shared default awaiting discovery, takes precedence.
The session fallback requires backend and profile ids; restore never synthesizes missing fields. A published backend must support the profile. A named device can await discovery, but an empty-id fallback requires an advertised compatible default.

The session records engine-confirmed identity, not consent. Its checkpoint does not write rerouting permission or change the preferred route. Restoring it grants no permission, even if the mapping contains `allowSystemOutputReroute` as an unknown key.

### Global TUI preferences

The `preferences` mapping in `<config>/tui.yaml` is owned by `Preferences` and its local `PreferencesSchema`. `version` is required, must be `1`, and is read before every other field; missing preference fields use the defaults below. Malformed present values reject the group.

| Field                     | Default                | Accepted values                                                                                    |
| ------------------------- | ---------------------- | -------------------------------------------------------------------------------------------------- |
| `language`                | empty string           | System (empty), `en`, `de`, `es`, `fr`, `ja`, `zh-Hans`, `zh-Hant`                                 |
| `coverArtMode`            | `auto`                 | `auto`, `kitty`, `blocks`, `off`                                                                   |
| `panelSeparator`          | `single`               | `single` (shared border), `double` (adjacent panel borders)                                        |
| `terminalTitleFormat`     | `$artist " - " $title` | A scalar [format expression](../query/format-language.md); empty disables terminal-title ownership |
| `terminalTitleSoul`       | `true`                 | Prefix the enabled terminal title with the shared three-cell Soul animation                        |
| `revealIndicatorsOnHover` | `false`                | Show panel arrows only while hovering their divider or collapsed edge                              |
| `dimBackdrop`             | `true`                 | Boolean                                                                                            |
| `reducedMotion`           | `false`                | Boolean                                                                                            |
| `mouseEnabled`            | `true`                 | Boolean                                                                                            |
| `qualityHover`            | `true`                 | Boolean                                                                                            |
| `wheelStep`               | `3`                    | Integer 1–10 tracks                                                                                |
| `seekSeconds`             | `5`                    | Integer 1–60 seconds                                                                               |
| `volumePercent`           | `5`                    | Integer 1–10 percentage points                                                                     |

If saved TUI preferences cannot be read or validated, startup uses defaults and posts a warning without resetting the store or rewriting that group; a group of a newer version posts the newer-version warning instead. A malformed document remains protected by `ConfigStore` write rejection; a later explicit Settings save can replace a schema-invalid preference group in an otherwise valid document, but never a group of a newer version.

Settings persists the complete candidate before publishing live behavior. A missing persistent location is a save failure. The one global store preserves `runtime`, `shortcuts`, and unknown sibling groups.

### Global shortcut group

The `shortcuts` group is a mapping of exactly two fields, a required `version`, which must be `1` and is read first, and the dynamic `bindings` mapping:

```text
version: 1
bindings: mapping<action-id string, sequence<key-chord string>>
```

Each present action id replaces the complete shipped chord list for that action.
An empty sequence explicitly unbinds it; an absent action id retains its current shipped defaults.
Saving writes only bindings whose effective chord sequence differs from the defaults supplied to `KeymapModel`.

GTK exposes a mutation surface and persists those deltas in its global document.
WinUI loads hand-authored overrides over the desktop defaults; it has no shortcut editor or shortcut writer.
GTK persists a shortcut candidate before publishing live accelerators, and the Keyboard page reports a failed `saveKeymap` result once.
TUI loads the same group from `<config>/tui.yaml` over its terminal defaults and saves acknowledged Settings Keyboard candidates before replacing live bindings. Ordinary shutdown does not rewrite untouched shortcuts.
Its normal output-preference checkpoint writes the `runtime` sibling through the same live `ConfigStore`, preserving the shortcut group already present in the store snapshot.

Action ids are plain strings in this payload.
Persistence accepts arbitrary action ids; the GTK editor derives shortcut eligibility from the layout schema, while TUI recognizes only its frontend descriptor inventory.
Loading the group itself does not reject an unknown action id.
Chord values use the canonical `KeyChord::toString()` representation; the exact chord tokens, aliases, and shipped default bindings belong to the [keyboard map reference](../shell/keymap.md).
The schema rejects a missing `bindings`, an unknown field, an empty action id, duplicate action id, non-sequence binding, null or non-scalar sequence element, or other malformed group structure as one failed candidate.
After structural acceptance, `KeymapModel` treats an unparseable chord string as an invalid semantic entry and continues with other usable chords.

### TUI sidebar widths

The `panels` group in `<root>/.aobus/tui_layout.yaml` stores required `version: 1`, `navigation`, and `detail` fields. Widths are nonnegative integers in terminal cells including the pane borders; zero selects automatic sizing. Missing or invalid groups use automatic sizing without discarding other layout groups. `PanelWidthsSchema` in `LayoutStateStore.cpp` owns this payload. Immediate keyboard steps, confirmed keyboard layout adjustments, and completed divider drags checkpoint widths atomically with the other layout groups. Keyboard-mode and drag previews, as well as terminal-size clamps, never overwrite these preferences.

### TUI List navigation

`<root>/.aobus/tui_layout.yaml` also contains `navigation: {version: 1, enabled: true}`. Both fields are required when the group exists. Missing, malformed, or unsupported state defaults to enabled; other valid layout groups remain independent. The same writer saves visibility, columns, and presentations atomically while preserving unrelated groups. Only explicit enable/disable changes request a visibility checkpoint; focus, search, expansion, terminal-size clamping, and chooser dismissal are not persistent. A failed save retains live/dirty state and reports a coalesced warning, with retry at the next checkpoint or normal exit.

### Delegated payload schemas

The registry fixes the group-to-type association, but these domain owners define payload fields and meaning:

| Payload                                                              | Current schema authority                                                                                                                                                                                                                                                                                     |
| -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `PlaybackSessionState`                                               | [Playback session persistence specification](../../system/playback/session-persistence.md), [state reference](../playback/session-state.md), and [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h).                                                                                    |
| `WorkspaceSessionDocument` / `WorkspaceSessionState`                 | [Workspace session state](../workspace/session-state.md).                                                                                                                                                                                                                                                    |
| `TrackColumnLayoutDocument` / `TrackColumnLayouts::Snapshot`         | [Persisted presentation state](../presentation/persisted-state.md) and [`TrackColumnLayoutYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h).                                                                                                                |
| `ListPresentationPreferenceDocument` / `ListPresentations::Snapshot` | [Persisted presentation state](../presentation/persisted-state.md), [list presentation preference specification](../../system/presentation/list-preference.md), and [`ListPresentationPreferenceYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h). |
| `LayoutDocument`                                                     | [Shell layout document](../shell/layout-document.md) and its model-specific YAML schema.                                                                                                                                                                                                                     |
| `LayoutComponentStateDocument`                                       | [Shell layout component state](../shell/layout-state.md) and its model-specific YAML schema.                                                                                                                                                                                                                 |

## Validation rules

- Registered group names are exact and case-sensitive.
- Global application files use `utility::applicationConfigDirectory`, which accepts only absolute candidates. Per-library files use `LibraryPaths` below the selected music root, while an explicitly selected workspace path remains owned by its frontend. No missing global-directory value falls back to the process working directory.
- A conforming `ConfigStore` file has a top-level mapping; each registered group is one unique keyed direct child, and duplicate group keys reject initialization.
- Unregistered top-level groups have no application consumer even though a loaded `ConfigStore` can retain them while rewriting another group.
- Every registered payload uses an explicit owner-local schema; no field, enum, id, container, or aggregate schema is inferred from its C++ type.
- The [grouped configuration store specification](../../system/persistence/config-store.md) owns explicit schema invocation, missing-group presence results, candidate isolation, and multi-group atomic replacement, not payload deserialization policy.
- `playback-session` accepts only schema version `1`, uses explicit enum and identifier mappings, and validates the complete structural and semantic candidate.
- `workspace` accepts only presentation version `3` and validates its complete stable presentation vocabulary before view creation. A load failure other than a missing group retains that group against later replacement on the same store; a successful decode does not. That retention is not protected configuration and not a migration.
- Interactive column-layout groups accept only version `1`; interactive
  list-presentation groups accept only version `1`. Each validates a complete
  candidate before replacing seeded UIModel state.
- Shell layout files require a readable `layout` group whose payload contains `version` and `root`; `templates` is optional.
- Shell component-state roots require `version`, `preset`, and `components`; each component entry requires `type`, `stateVersion`, `baselineHash`, and `state`.
- A component-state file name's preset id must equal the deserialized root `preset` value before the store returns the document.

The domain owner decides whether a syntactically deserialized identity, enum ordinal, version marker, or nested value is usable.
This registry does not convert schema membership into restore success.

### Frontend policy without a persistent application location

The [grouped configuration store specification](../../system/persistence/config-store.md#sessions-with-no-location) owns the `NoLocation` mechanism: empty initial groups, same-instance live writes, candidate isolation and exception behavior, successful generic saves with no filesystem access, and the prohibition on an empty-path working-directory fallback.
This section owns only frontend choices layered over that mechanism.

`utility::applicationConfigDirectory` returns `NotFound` only when the platform names no home or profile location.
GTK then opens its global `AppConfigStore` and shell-layout stores without a location and continues with survivable defaults.
TUI does the same for its separate global application store both when that resolver fails and when creating the resolved application's configuration directory fails.
Its command-line-selected per-library workspace/playback path is independent: failure to prepare that path is a startup failure, not a no-location application-preference session.
The Windows shell does not degrade this way because its application directory also owns required settings, playback state, and the fallback library root; failure to resolve or prepare it rejects startup.

TUI distinguishes passive checkpoints from an explicit Settings acknowledgement.
Its `preferences` save and `shortcuts` save both check `hasLocation()` and return `NotFound` before serialization when the application store has no location; Settings therefore does not claim that the user's requested change was persisted or publish that candidate as live behavior.
Other owners that call generic store saves retain the store-level success and same-instance visibility contract.
This is an intentional policy difference, not a requirement to weaken explicit Settings saves into volatile success.
The composition root logs the degraded application-preference session once, where its cause is known; an explicit Settings save still reports its own rejected acknowledgement.

## Compatibility and versioning

| Surface                       | Compatibility mechanism                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ----------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `window` and `session`        | No explicit version or migration. Their schemas retain seeded values for missing known fields and tolerate unknown fields while rejecting duplicates and malformed known fields.                                                                                                                                                                                                                                                                                                                                                                                                                |
| `runtime`                     | Required version `1`, read first. A protected group; see the runtime field rules for seeded ids and fail-closed rerouting permission.                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `shortcuts`                   | Required version `1`, read first, and strict group fields. Action ids are dynamic; the schema strictly validates the mapping/sequence/scalar structure, and keymap semantics handle unknown actions and invalid chord text. A protected group.                                                                                                                                                                                                                                                                                                                                                  |
| TUI `preferences`             | Required payload version `1`, read first, strict field membership and value validation, defaults for missing preference fields, and no earlier-version migration. A protected group.                                                                                                                                                                                                                                                                                                                                                                                                            |
| `workspace`                   | Nested presentation vocabulary version `3`, strict deserialization, stable textual ids, and no migration. A failed load keeps the group until explicit removal; that retention is not a protected-group refusal. The [workspace session state reference](../workspace/session-state.md) and [session failure rules](../../system/workspace/session.md#failure-execution-and-lifetime) own the remaining limits.                                                                                                                                                                                 |
| `trackView.columnLayouts`     | Payload version `1` only; also reads the earlier boolean spelling. See the [column-layout format](../presentation/persisted-state.md#column-layout-group).                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `trackView.presentations`     | Independent payload version `1`, strict deserialization, stable text identities, and no unversioned migration.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| TUI `navigation` and `panels` | Independent payload version `1` for each group, strict field membership, and no earlier-version migration. Invalid navigation falls back to enabled; invalid panel widths fall back to automatic sizing at the owning workflow.                                                                                                                                                                                                                                                                                                                                                                 |
| Windows `desktop`             | Explicit version, currently `1`, read down to the oldest this schema ever wrote (`1`). Every field is optional over the caller's seed, so an accepted older document is read in full and rewritten at the current version on the next checkpoint. A newer version is rejected and kept unchanged because this build cannot preserve fields it does not know; a version below the oldest is rejected because no document was written that way and the value marks a malformed one. A protected group. Exact fields belong to the [Windows desktop state reference](../windows/desktop-state.md). |
| `playback-session`            | Explicit schema version `1`; other versions are rejected rather than migrated.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `layout`                      | Required version `1`; unsupported versions are rejected before the root or templates are interpreted. No legacy or reflected fallback is attempted. A protected group. `ShellLayoutStore` also refuses to remove a preset file of a newer version, or one whose bytes it cannot read.                                                                                                                                                                                                                                                                                                           |
| Shell component state         | Required file version `1` and entry version `1`; unsupported versions are rejected before version-specific payload interpretation. No legacy fallback is attempted.                                                                                                                                                                                                                                                                                                                                                                                                                             |

A protected group of a newer version is neither read nor written: the store refuses every load and save of it, and every removal once a load or save has found it newer, as the [grouped configuration store](../../system/persistence/config-store.md#protected-groups) specifies. The build keeps its defaults for that group for the session.
A rejected `workspace` load is not that mechanism. The session keeps those bytes until `removeGroup`, removal is allowed, and the refusal is `InvalidState` rather than `NotSupported`. See [workspace session failure](../../system/workspace/session.md#failure-execution-and-lifetime).
The frontend posts one warning when it loads such a group, as the [versioning page](../../system/persistence/versioning.md#protected-configuration) states.

Within unreleased `runtime` version `1`, development builds that do not recognize `allowSystemOutputReroute` ignore it on read and may drop it when replacing the group. This same-version compatibility rule does not override [newer-version write protection](../../decision/0020-versioning-and-upgrade-policy.md).

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
  version: 1
  lastOutputBackendId: pipewire
  lastOutputProfileId: shared
  lastOutputDeviceId: "node:speakers"
  lastLayoutPreset: classic
  lastThemePreset: classic
  allowSystemOutputReroute: false
session:
  lastLibraryPath: /music
  lastOutputBackendId: pipewire
  lastOutputProfileId: shared
  lastOutputDeviceId: "node:speakers"
shortcuts:
  version: 1
  bindings:
    playback.playPause:
      - Ctrl+Shift+P
    playback.cycleRepeat: []
```

Mapping order is not significant.
The example intentionally omits the domain-owned `playback-session` payload.

## Implementation authority

- [`Preferences.h`](../../../app/tui/Preferences.h) owns the TUI preference payload and defaults; [`Preferences.cpp`](../../../app/tui/Preferences.cpp) owns the `preferences` schema, version gate, and group reads and writes.
- [`AppState.cpp`](../../../app/runtime/AppState.cpp) owns the `runtime` and `session` schemas, including when saved output ids can grant rerouting permission. [`AppConfigStore.cpp`](../../../app/linux-gtk/app/AppConfigStore.cpp) groups the GTK saves, and [`WindowState.h`](../../../app/linux-gtk/app/WindowState.h) defines the geometry payload; [`WindowState.cpp`](../../../app/linux-gtk/app/WindowState.cpp) owns the geometry accumulation rule and [`MainWindow.cpp`](../../../app/linux-gtk/app/MainWindow.cpp) owns when a checkpoint is taken.
- [`ConfigStore.h`](../../../app/include/ao/rt/ConfigStore.h) owns the generic `NoLocation` mechanism; [`PlatformDirectories.h`](../../../include/ao/utility/PlatformDirectories.h) owns application-directory resolution. [`Preferences.cpp`](../../../app/tui/Preferences.cpp) and [`app/tui/App.cpp`](../../../app/tui/App.cpp) own the stricter explicit TUI Settings saves and the TUI's additional no-location fallback when application-directory creation fails.
- [`KeymapStore.h`](../../../app/include/ao/uimodel/input/KeymapStore.h) and [`KeymapModel.h`](../../../app/include/ao/uimodel/input/KeymapModel.h) own the shortcut group name and mapping payload; [`Keymap.h`](../../../app/tui/Keymap.h) owns the terminal defaults supplied when the TUI reads it.
- [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h), [`PlaybackSessionYamlSchema.h`](../../../app/runtime/PlaybackSessionYamlSchema.h), [`PlaybackSessionYamlSchema.cpp`](../../../app/runtime/PlaybackSessionYamlSchema.cpp), and [`PlaybackSessionPersistence.cpp`](../../../app/runtime/PlaybackSessionPersistence.cpp) own the playback group, explicit schema, payload marker, and injected-store use.
- [`WorkspaceSessionYamlSchema.h`](../../../app/runtime/WorkspaceSessionYamlSchema.h), [`WorkspaceSessionYamlSchema.cpp`](../../../app/runtime/WorkspaceSessionYamlSchema.cpp), and [`WorkspaceService.cpp`](../../../app/runtime/WorkspaceService.cpp) own the workspace group and payload conversion.
- [`GtkLayoutStateStore.cpp`](../../../app/linux-gtk/app/GtkLayoutStateStore.cpp), [`LayoutStateStore.cpp`](../../../app/tui/LayoutStateStore.cpp), and [`LibrarySession.cpp`](../../../app/windows-winui/app/LibrarySession.cpp) own their independent library-presentation file boundaries; the TUI store also owns the `navigation` and `panels` schemas.
- [`TrackColumnLayoutYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h) and [`ListPresentationPreferenceYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h) own the shared literal group names and exact presentation payloads.
- [`ShellLayoutStore.cpp`](../../../app/linux-gtk/app/ShellLayoutStore.cpp) owns the layout-preset group and file boundary.
- [`LayoutComponentState.h`](../../../app/include/ao/uimodel/layout/component/LayoutComponentState.h), [`LayoutComponentState.cpp`](../../../app/uimodel/layout/component/LayoutComponentState.cpp), and [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp) own the standalone component-state envelope and markers.
- [`app/linux-gtk/main.cpp`](../../../app/linux-gtk/main.cpp), [`AppRuntime.cpp`](../../../app/runtime/AppRuntime.cpp), and [`app/tui/App.cpp`](../../../app/tui/App.cpp) own store selection and sharing.
- [`NewerConfigNotice.h`](../../../app/include/ao/uimodel/preference/NewerConfigNotice.h) owns the warning for a protected group of a newer version; each frontend picks the member's message.
- [`DesktopSettingsYamlSchema`](../../../app/windows-winui/include/ao/winui/DesktopSettingsYamlSchema.h)
  owns the Windows `desktop` payload, while WinUI `LibrarySession` owns its
  checkpoint and restore lifecycle.

## Test authority

- [`PreferencesTest.cpp`](../../../test/tui/PreferencesTest.cpp) protects TUI preference defaults, validation, version rejection, a newer group kept unchanged, round trips, and preservation of sibling groups.
- [`AppStateTest.cpp`](../../../test/runtime/AppStateTest.cpp) protects the shared `runtime` and `session` schemas, including scoped routing permission, fixed defaults, a missing `version`, and a newer `runtime` group kept unchanged.
- [`AppConfigStoreTest.cpp`](../../../test/linux-gtk/app/AppConfigStoreTest.cpp) protects the `window`, `runtime`, and `session` group round trips, missing-file behavior, and the no-location session.
- [`WindowStateTest.cpp`](../../../test/linux-gtk/app/WindowStateTest.cpp) protects current-session normal geometry across a maximized checkpoint.
- [`ConfigStoreTest.cpp`](../../../test/runtime/ConfigStoreTest.cpp) protects what a store with no location reads, writes, and leaves out of the working directory, and the protected-group refusals.
- [`NewerConfigNoticeTest.cpp`](../../../test/uimodel/preference/NewerConfigNoticeTest.cpp) protects the warning for a protected group of a newer version: one retained warning per member; [`ShellLayoutControllerTest.cpp`](../../../test/linux-gtk/app/ShellLayoutControllerTest.cpp) protects it for a newer layout preset at startup and in the layout editor.
- [`KeymapStoreTest.cpp`](../../../test/uimodel/input/KeymapStoreTest.cpp) protects the versioned `shortcuts` group, a newer group kept unchanged, merge, and delta-only persistence; [`KeymapTest.cpp`](../../../test/tui/KeymapTest.cpp) protects the independent terminal defaults and their executable projection.
- [`PlaybackSessionTest.cpp`](../../../test/runtime/PlaybackSessionTest.cpp) protects the exact `playback-session` field set, schema version, store use, and repeated frozen checkpoints.
- [`WorkspaceSessionTest.cpp`](../../../test/runtime/WorkspaceSessionTest.cpp) and [`HeadlessShellTest.cpp`](../../../test/runtime/HeadlessShellTest.cpp) protect the `workspace` group and frontend-neutral round trip; [`LibraryControllerTest.cpp`](../../../test/tui/LibraryControllerTest.cpp) protects TUI attachment to the exact restored active view.
- [`TrackColumnLayoutYamlSchemaTest.cpp`](../../../test/uimodel/library/presentation/TrackColumnLayoutYamlSchemaTest.cpp), [`ListPresentationPreferenceYamlSchemaTest.cpp`](../../../test/uimodel/library/presentation/ListPresentationPreferenceYamlSchemaTest.cpp), [`GtkLayoutStateStoreTest.cpp`](../../../test/linux-gtk/app/GtkLayoutStateStoreTest.cpp), and [`LayoutStateStoreTest.cpp`](../../../test/tui/LayoutStateStoreTest.cpp) protect the shared presentation groups, independent files, TUI writer-path alias rejection, version gates, and seeded-state fallback.
- [`LayoutModelTest.cpp`](../../../test/uimodel/layout/document/LayoutModelTest.cpp) protects the layout payload's YAML fields and round trip; [`ShellLayoutStoreTest.cpp`](../../../test/linux-gtk/app/ShellLayoutStoreTest.cpp) protects its `layout` group and per-preset file boundary.
- [`LayoutComponentStateTest.cpp`](../../../test/uimodel/layout/component/LayoutComponentStateTest.cpp) protects the standalone component-state envelope, versions, and schema; [`ShellLayoutComponentStateStoreTest.cpp`](../../../test/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp) protects preset matching, pruning, and the file boundary.
- [`DesktopSettingsYamlSchemaTest.cpp`](../../../test/winui/DesktopSettingsYamlSchemaTest.cpp)
  protects the Windows `desktop` field set, version gate, and output-selection
  round trip.

No single test currently enumerates every registered group across all logical documents; this reference is checked against the individual authorities above.

## Related documents

- [Persistence and managed-state architecture](../../system/persistence/README.md)
- [Grouped configuration store specification](../../system/persistence/config-store.md)
- [Atomic file replacement specification](../../system/persistence/atomic-replacement.md)
- [Managed file locations reference](location.md)
- [Workspace session state](../workspace/session-state.md)
- [Workspace session specification](../../system/workspace/session.md)
- [List presentation preference specification](../../system/presentation/list-preference.md)
- [Persisted presentation state](../presentation/persisted-state.md)
- [Application shell architecture](../../system/shell/README.md)
- [Shell layout lifecycle specification](../../system/shell/layout-lifecycle.md)
- [Keyboard shortcut specification](../../system/shell/keyboard-shortcut.md)
