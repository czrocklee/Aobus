---
id: presentation.persisted-state
type: reference
status: current
domain: presentation
summary: Enumerates versioned GTK presentation documents, stable token authorities, exact fields, validation, and compatibility behavior.
---
# Persisted presentation state

## Scope and version

This reference owns the exact version-1 payloads stored in the GTK per-library `trackView.columnLayouts` and `trackView.presentations` groups.
It also routes the stable textual vocabulary shared with nested workspace presentation state.

The [workspace session-state reference](../workspace/session-state.md) owns the exact workspace group.
The [application managed-state surface](../persistence/application-config.md) owns group-to-document registration and writer identity, while presentation specifications own runtime behavior and fallback.

Version 1 is the first supported format.
There is no legacy numeric migration.

## Code boundary

This surface spans the application runtime, UIModel, and GTK persistence-adapter layers from the [system architecture](../../architecture/system-overview.md), as refined by the [presentation architecture](../../architecture/presentation.md) and [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md).
Stable `TrackField`, `TrackSortField`, and `TrackGroupKey` ids belong to application runtime in `TrackField.h` and `TrackField.cpp`.
The two GTK payload models and semantic converters belong to UIModel in `TrackColumnLayoutYamlSchema` and `ListPresentationPreferenceYamlSchema`.
`GtkLayoutStateStore` owns the per-library file and literal group names but does not redefine either payload.

## Stable vocabulary

The [runtime track-field catalog](../library/model/track-field.md) is the exhaustive authority for field, sort-field, and group-key ids.
Tokens are lowercase, case-sensitive strings and are independent of C++ enum names and raw values.

Workspace sort directions use exactly:

| Token | Meaning |
|---|---|
| `ascending` | Apply the sort field in ascending order. |
| `descending` | Apply the sort field in descending order. |

Presentation and preset ids are nonempty opaque strings.
Built-in ids are enumerated by the [track-preset reference](track-preset.md); custom ids may be temporarily unavailable and are not a closed enum vocabulary.

## GTK column-layout group

The literal top-level group is `trackView.columnLayouts`.
Its exact shape is:

```yaml
trackView.columnLayouts:
  version: 1
  layouts:
    - listId: 10
      columns:
        - field: artist
          width: -1
          weight: 1.75
        - field: duration
          width: 200
          weight: -1
```

### Root fields

| Field | Type | Requirement |
|---|---|---|
| `version` | Unsigned 32-bit integer. | Required and exactly `1`. |
| `layouts` | Sequence of layout mappings. | Required; may be empty. |

### Layout fields

| Field | Type | Requirement |
|---|---|---|
| `listId` | Unsigned 32-bit integer. | Required, nonzero, and unique in the document. |
| `columns` | Ordered sequence of column mappings. | Required; field ids must be unique within this layout. |

### Column fields

| Field | Type | Requirement |
|---|---|---|
| `field` | Stable track-field id. | Required and known. |
| `width` | Signed 32-bit integer. | Fixed state uses a positive value; flexible state uses `-1`. |
| `weight` | Floating-point number. | Fixed state uses `-1`; flexible state uses a finite positive value. |

Exactly one canonical dimension form is accepted:

- fixed: `width > 0` and `weight == -1`;
- flexible: `width == -1` and finite `weight > 0`.

## GTK list-presentation preference group

The literal top-level group is `trackView.presentations`.
Its exact shape is:

```yaml
trackView.presentations:
  version: 1
  preferences:
    - listId: 10
      presentationId: albums
```

### Root fields

| Field | Type | Requirement |
|---|---|---|
| `version` | Unsigned 32-bit integer. | Required and exactly `1`. |
| `preferences` | Sequence of preference mappings. | Required; may be empty. |

### Preference fields

| Field | Type | Requirement |
|---|---|---|
| `listId` | Unsigned 32-bit integer. | Required, nonzero, and unique in the document. |
| `presentationId` | String. | Required and nonempty. It need not resolve in the current catalog. |

An unavailable `presentationId` survives deserialization and follows the recommendation fallback in the [list-preference specification](../../spec/presentation/list-preference.md).
This extensible-reference rule does not apply to closed field, sort, group, or direction tokens.

## Workspace relationship

The workspace group carries `presentationVersion: 1` and uses the same stable field, sort, group, direction, and preset-id vocabulary.
Its root and nested fields are exhaustively defined by the [workspace session-state reference](../workspace/session-state.md).

`presentationVersion` versions only the nested presentation serialization.
It is not the complete workspace schema version proposed by [RFC 0017](../../rfc/0017-versioned-workspace-session.md).

## Validation rules

- Both GTK groups use explicit UIModel schemas that validate each mapping and sequence recursively.
- Every declared field is required; unknown fields, missing fields, wrong node kinds, malformed vector elements, and unsupported versions reject the complete containing group.
- Semantic conversion occurs into a temporary candidate; no column or preference entry is installed before the whole group succeeds.
- A missing or rejected group leaves the caller's seeded state unchanged.
- The two groups are independent on load, so rejection of one does not reject a valid sibling group.
- The GTK coordinator suppresses save callbacks while installing loaded candidates, so a valid sibling cannot overwrite a rejected group during restore.
- Serialization rejects invalid live list ids, empty required ids, duplicate fields, and noncanonical column dimensions.
- `GtkLayoutStateStore` submits both schemas through one `saveTogether()` candidate, so a serialization failure cannot persist only one new group.

## Compatibility and versioning

Writers emit only version 1 with stable text ids and canonical member names.
Unversioned legacy state, numeric field values, unknown closed tokens, and future versions are rejected without an automatic rewrite.
Future versions return `NotSupported` before version-specific sibling fields are interpreted.

Changing a stable token's meaning or spelling requires an explicit compatibility decision.
Adding a token does not change the meaning of existing version-1 documents, but older readers reject a document that uses the new unknown value.

The surrounding `gtk_layout.yaml` file has no shared envelope version.
Each literal group carries and gates its own payload version.

## Implementation authority

- [`TrackField.h`](../../../app/include/ao/rt/TrackField.h) and [`TrackField.cpp`](../../../app/runtime/TrackField.cpp) own stable token conversion.
- [`TrackColumnLayoutYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h) and [`TrackColumnLayoutYamlSchema.cpp`](../../../app/uimodel/library/presentation/TrackColumnLayoutYamlSchema.cpp) own the layout document and conversion.
- [`ListPresentationPreferenceYamlSchema.h`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h) and [`ListPresentationPreferenceYamlSchema.cpp`](../../../app/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.cpp) own the preference document and conversion.
- [`GtkLayoutStateStore.cpp`](../../../app/linux-gtk/app/GtkLayoutStateStore.cpp) owns group selection, load policy, and the file save boundary.

## Test authority

- [`TrackFieldTest.cpp`](../../../test/unit/runtime/TrackFieldTest.cpp) proves stable token coverage, uniqueness, and round trip.
- [`TrackColumnLayoutYamlSchemaTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnLayoutYamlSchemaTest.cpp) protects layout conversion and rejection.
- [`ListPresentationPreferenceYamlSchemaTest.cpp`](../../../test/unit/uimodel/library/presentation/ListPresentationPreferenceYamlSchemaTest.cpp) protects opaque preference ids and rejection.
- [`GtkLayoutStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp) protects exact group integration, seeded fallback, and canonical output.
- [`WorkspaceSessionYamlSchemaTest.cpp`](../../../test/unit/runtime/WorkspaceSessionYamlSchemaTest.cpp) protects the shared workspace vocabulary.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Track-column layout](../../spec/presentation/track-column-layout.md)
- [List presentation preference](../../spec/presentation/list-preference.md)
- [Runtime track-field catalog](../library/model/track-field.md)
- [Track presentation presets](track-preset.md)
- [Workspace session state](../workspace/session-state.md)
- [Application managed-state surface](../persistence/application-config.md)
