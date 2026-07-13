---
id: workspace.session-state
type: reference
status: current
domain: workspace
summary: Enumerates the unversioned workspace configuration group, reflected fields, nested view and presentation values, defaults, and compatibility limits.
---
# Workspace session state

## Scope and version

This reference owns the exact current payload of the `workspace` configuration group.
It enumerates `WorkspaceSessionState`, its nested view configurations and custom presentation presets, their reflected field names, YAML shapes, defaults, and current compatibility limits.

The payload has no schema-version field and no migration layer.
The [application managed-state surface](../persistence/application-config.md) owns the registry that associates this payload with a logical document and writer; the [workspace session specification](../../spec/workspace/session.md) owns save, restore, fallback, and failure behavior.

## Code boundary

The payload belongs to the **application runtime** layer in the [system architecture](../../architecture/system-overview.md), under the [workspace](../../architecture/workspace.md) and [persistence and managed-state](../../architecture/persistence-and-managed-state.md) architectures.

The model is public under `app/include/ao/rt/WorkspaceSessionState.h`, `ViewState.h`, `TrackPresentation.h`, and `TrackField.h`.
The runtime `ConfigStore` and `app/include/ao/yaml/ConfigTraits.h` encode it as one ordinary reflected aggregate; frontends select the containing file but do not redefine the payload.

## Surface

### Group and root

The literal top-level group name is `workspace`.
Its value is one mapping with these reflected fields in current emitted order:

| Field | YAML shape | C++ type | Default |
|---|---|---|---|
| `openViews` | Sequence of mappings. | `std::vector<TrackListViewConfig>` | Empty sequence. |
| `activeListId` | Unsigned 32-bit scalar. | `ListId` | `0`, the invalid list id. |
| `customPresets` | Sequence of mappings. | `std::vector<CustomTrackPresentationPreset>` | Empty sequence. |

The session stores no view id.
`activeListId` is a focus hint interpreted against the restored `openViews` sequence rather than a unique serialized view identity.

### Track-list view configuration

Every `openViews` entry is a mapping with these reflected fields:

| Field | YAML shape | C++ type | Default and meaning |
|---|---|---|---|
| `listId` | Unsigned 32-bit scalar. | `ListId` | `4294967295`, the All Tracks virtual list id. |
| `filterExpression` | Scalar string. | `std::string` | Empty; no transient filter. |
| `groupBy` | Signed 32-bit scalar. | `TrackGroupKey` | `0`, `None`. |
| `sortBy` | Sequence of `TrackSortTerm` mappings. | `std::vector<TrackSortTerm>` | Empty. |
| `optPresentation` | `TrackPresentationSpec` mapping or YAML null. | `std::optional<TrackPresentationSpec>` | Null. |

Current save always supplies `optPresentation` from the live view while also writing `groupBy` and `sortBy`.
During restore, a present presentation is normalized and takes precedence over the legacy top-level group/sort fields.
When `optPresentation` is absent or null, view construction derives presentation from `groupBy`, `sortBy`, list kind, and built-in defaults.

### Sort term

Every `sortBy` entry is a mapping:

| Field | YAML shape | C++ type | Default |
|---|---|---|---|
| `field` | Signed 32-bit scalar. | `TrackSortField` | `13`, `Title`. |
| `ascending` | Boolean scalar. | `bool` | `true`. |

The exact raw values for `TrackSortField` and `TrackGroupKey` are owned by the [sort-field](../library/model/track-field.md#sort-fields) and [group-key](../library/model/track-field.md#group-keys) tables in the runtime track field catalog.

### Track presentation

`optPresentation` and each custom preset's `spec` use this mapping:

| Field | YAML shape | C++ type | Default |
|---|---|---|---|
| `id` | Scalar string. | `std::string` | Empty. |
| `groupBy` | Signed 32-bit scalar. | `TrackGroupKey` | `0`, `None`. |
| `sortBy` | Sequence of `TrackSortTerm` mappings. | `std::vector<TrackSortTerm>` | Empty. |
| `visibleFields` | Sequence of signed 32-bit scalars. | `std::vector<TrackField>` | Empty. |
| `redundantFields` | Sequence of signed 32-bit scalars. | `std::vector<TrackField>` | Empty. |

The exact raw `TrackField` values belong to the [runtime track field catalog](../library/model/track-field.md).
Built-in presentation ids and their current semantic defaults belong to the [track presentation preset reference](../presentation/track-preset.md).

### Custom presentation preset

Every `customPresets` entry is a mapping:

| Field | YAML shape | C++ type | Default |
|---|---|---|---|
| `label` | Scalar string. | `std::string` | Empty. |
| `basePresetId` | Scalar string. | `std::string` | Empty. |
| `spec` | `TrackPresentationSpec` mapping. | `TrackPresentationSpec` | Default-constructed mapping. |

The codec accepts strings independently of the current built-in or custom presentation catalog.
Workspace behavior later resolves ids and applies presentation normalization.

## Validation rules

- The group value must be a mapping when present.
- `openViews`, `customPresets`, every `sortBy`, and both field collections must be YAML sequences when present.
- Strong list ids must parse within the unsigned 32-bit range.
- Enum values must parse as signed 32-bit integers; the generic codec does not reject values outside the declared enum domains.
- Boolean values use the shared canonical scalar parser.
- `optPresentation` accepts a mapping or null.
- Ordinary aggregate decoding ignores unknown fields and leaves absent fields at the seeded/default target value.
- Ordinary vector decoding skips an element that cannot decode and continues with later elements.
- No codec-level limit exists for view count, preset count, string length, sort-term count, or field count.
- The codec does not require a nonzero list id, a present library list, a known presentation id, unique custom ids, or internally consistent group/sort/presentation values.

Semantic usability is decided during workspace and view restoration under the [workspace session specification](../../spec/workspace/session.md).
Generic parse, node, scalar, and ordinary-decode behavior belongs to the [grouped configuration store specification](../../spec/persistence/config-store.md).

## Compatibility and versioning

There is no workspace payload version, document-kind marker, migration registry, field alias, or compatibility window.
Current serialization derives mapping keys from C++ aggregate member names and enums from their numeric definitions.

Consequently:

- renaming an aggregate member changes its emitted key, while reordering members changes emitted mapping order but not key-based decode meaning;
- changing enum raw values changes persisted meaning;
- missing newer fields fall back to current C++ defaults;
- unknown older fields are ignored on load and are not retained when the `workspace` group is rewritten;
- malformed vector elements can disappear during permissive load rather than rejecting the complete session;
- `activeListId` cannot distinguish two restored views over the same base list beyond the current focus-selection heuristic.

[RFC 0010](../../rfc/0010-versioned-presentation-state.md) proposes a versioned presentation-state codec and migration boundary.
[RFC 0015](../../rfc/0015-fail-closed-config-store.md) proposes result-bearing grouped transactions.
[RFC 0017](../../rfc/0017-versioned-workspace-session.md) proposes a library-bound root envelope, exact session-local active-view identity, bounded strict validation, and integration with both proposals; none changes this current reference until implemented.

## Examples

This example contains one filtered All Tracks view and one custom presentation:

```yaml
workspace:
  openViews:
    - listId: 4294967295
      filterExpression: '$genre = "Classical"'
      groupBy: 2
      sortBy:
        - field: 1
          ascending: true
        - field: 12
          ascending: true
      optPresentation:
        id: albums
        groupBy: 2
        sortBy:
          - field: 1
            ascending: true
        visibleFields:
          - 0
          - 1
          - 2
        redundantFields:
          - 2
  activeListId: 4294967295
  customPresets:
    - label: Works
      basePresetId: library
      spec:
        id: custom.works
        groupBy: 8
        sortBy:
          - field: 7
            ascending: true
        visibleFields:
          - 0
          - 4
        redundantFields: []
```

Mapping order is not semantically significant even though current reflected emission follows declaration order.
The example's numeric field, sort, and group values are interpreted through the runtime track-field catalog.

## Implementation authority

- [`WorkspaceSessionState.h`](../../../app/include/ao/rt/WorkspaceSessionState.h) owns the root field set and defaults.
- [`ViewState.h`](../../../app/include/ao/rt/ViewState.h) owns `TrackListViewConfig`.
- [`TrackPresentation.h`](../../../app/include/ao/rt/TrackPresentation.h) owns presentation and custom-preset fields.
- [`TrackField.h`](../../../app/include/ao/rt/TrackField.h) owns sort terms and enum definitions.
- [`ConfigTraits.h`](../../../app/include/ao/yaml/ConfigTraits.h) owns reflected keys and scalar, optional, vector, enum, strong-id, and aggregate encodings.
- [`WorkspaceService.cpp`](../../../app/runtime/WorkspaceService.cpp) owns the literal group name and snapshot construction.

## Test authority

- [`WorkspaceSessionTest.cpp`](../../../test/unit/runtime/WorkspaceSessionTest.cpp) protects missing, malformed, fallback, and restoration outcomes.
- [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) protects workspace group round trips and presentation reconstruction across runtime instances.
- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) protects the common reflected aggregate, vector, optional, enum, and strong-id codec behavior.
- [`WorkspacePresentationTest.cpp`](../../../test/unit/runtime/WorkspacePresentationTest.cpp) protects custom-preset runtime behavior after decoding.

No focused fixture currently locks every workspace YAML key and numeric enum value as one complete schema snapshot.
The C++ aggregate declarations and shared codec therefore remain the exact implementation authority.

## Related documents

- [Workspace architecture](../../architecture/workspace.md)
- [Workspace session specification](../../spec/workspace/session.md)
- [Workspace navigation specification](../../spec/workspace/navigation.md)
- [Application managed-state surface](../persistence/application-config.md)
- [Managed file locations](../persistence/location.md)
- [Grouped configuration store](../../spec/persistence/config-store.md)
- [Runtime track field catalog](../library/model/track-field.md)
- [Track presentation presets](../presentation/track-preset.md)
