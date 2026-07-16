---
id: workspace.session-state
type: reference
status: current
domain: workspace
summary: Enumerates the strict workspace group, versioned presentation vocabulary, exact fields, validation, and remaining compatibility limits.
---
# Workspace session state

## Scope and version

This reference owns the exact current payload of the `workspace` configuration group.
It enumerates the private persistence document, nested view presentations and custom presets, stable values, strict validation, and compatibility behavior.

The required `presentationVersion` is currently `1`.
It versions the nested presentation vocabulary, not the complete workspace document.
The payload still has no root workspace schema version or migration registry; [RFC 0017](../../rfc/0017-versioned-workspace-session.md) owns that broader proposal.

The [workspace session specification](../../spec/workspace/session.md) owns capture, candidate creation, focus fallback, commit, and failures after decoding.
The [application managed-state surface](../persistence/application-config.md) owns document and writer registration.

## Code boundary

This surface belongs to the application runtime layer from the [system architecture](../../architecture/system-overview.md), as refined by the [workspace architecture](../../architecture/workspace.md) and [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md).
`WorkspaceSessionState` remains the runtime semantic candidate.
The private `WorkspaceSessionDocument` and nested stored DTOs in `app/runtime/WorkspaceSessionCodec.h` are the exact YAML model.
`WorkspaceSessionCodec.cpp` converts between that document and runtime `TrackListViewConfig`, `TrackPresentationSpec`, and custom-preset values.

`WorkspaceService` selects the literal `workspace` group and uses `ConfigStore::loadExact` before semantic conversion.
Frontends select the containing path but do not decode or redefine fields.

## Surface

### Group root

The literal top-level group is `workspace`.
Its value is one mapping with these required fields in canonical emitted order:

| Field | YAML shape | Meaning |
|---|---|---|
| `presentationVersion` | Unsigned 32-bit scalar. | Required and exactly `1`. |
| `openViews` | Sequence of view mappings. | Ordered semantic view reconstruction records. |
| `activeListId` | Unsigned 32-bit scalar. | Advisory focus hint; `0` is invalid/no hint. |
| `customPresets` | Sequence of custom-preset mappings. | Complete workspace custom-preset collection. |

The session stores no `ViewId`.
`activeListId` cannot uniquely identify two views over the same base list and follows the focus heuristic in the workspace session specification.

### Track-list view entry

Every `openViews` entry contains:

| Field | YAML shape | Requirement and meaning |
|---|---|---|
| `listId` | Unsigned 32-bit scalar. | Required and nonzero; identifies the base library or virtual list. |
| `filterExpression` | String. | Required; empty means no transient filter. |
| `presentation` | Presentation mapping. | Required exact presentation used to reconstruct the view. |

Legacy top-level `groupBy`, `sortBy`, and optional presentation fields are not part of version 1.
Grouping and sorting occur only inside the required exact presentation.

### Presentation mapping

Each view `presentation` and custom-preset `spec` contains:

| Field | YAML shape | Requirement |
|---|---|---|
| `id` | String. | Required and nonempty; opaque presentation identity. |
| `group` | Stable group-key string. | Required and known. |
| `sort` | Sequence of sort-term mappings. | Required; sort fields must be unique. |
| `visibleFields` | Sequence of stable track-field strings. | Required, nonempty, known, and unique. |
| `redundantFields` | Sequence of stable track-field strings. | Required, known, and unique. |

The stable token catalogs are owned by the [runtime track-field catalog](../library/model/track-field.md) and routed through the [persisted presentation-state reference](../presentation/persisted-state.md).
Presentation ids are not resolved against the current catalog during decoding.

### Sort term

Every `sort` entry contains:

| Field | YAML shape | Requirement |
|---|---|---|
| `field` | Stable sort-field string. | Required, known, and unique within the presentation. |
| `direction` | String. | Required; exactly `ascending` or `descending`. |

### Custom presentation preset

Every `customPresets` entry contains:

| Field | YAML shape | Meaning |
|---|---|---|
| `label` | String. | User-visible label. |
| `basePresetId` | String. | Opaque source/base preset identity; may be empty. |
| `spec` | Presentation mapping. | Complete version-1 presentation spec. |

## Validation rules

- The group uses strict recursive aggregate/vector decoding; every listed member is required.
- Unknown members, missing members, wrong node kinds, malformed scalar values, and malformed vector elements reject the whole workspace document.
- `presentationVersion` must be `1`.
- View list ids must be nonzero; existence in the active library is checked while candidate views are created.
- Closed field, sort, group, and direction tokens must resolve exactly and case-sensitively.
- Duplicate sort fields and duplicate values within either field collection reject the document.
- Presentation ids must be nonempty and decoded presentations must have at least one visible field.
- Encoding requires every live view to supply an exact presentation and rejects invalid enum values, duplicate sort fields, invalid list ids, and empty ids as `InvalidState`.
- Encoding normalizes permitted live defaults, deduplicates field collections, and supplies `title` for an empty visible set, so canonical output always has a nonempty `visibleFields` sequence.
- Structural and semantic decode completes before `WorkspaceService` creates or installs candidate views.

There is no codec-level limit for view count, preset count, string length, sort-term count, or field count.
Library binding, resource budgets, exact active-view identity, and complete candidate-set validation remain RFC 0017 concerns.

## Compatibility and versioning

Version 1 is the first supported stable presentation encoding.
Unversioned reflected workspace state, numeric enums, unknown closed tokens, and unsupported presentation versions are rejected without migration or automatic rewrite.

Strict decoding fixes the current root member set, so changing those keys cannot remain an unnoticed version-1 edit.
However, `presentationVersion` promises only the nested presentation vocabulary; it supplies no document kind, library binding, filter-expression dialect contract, exact active-view identity, collection budgets, or root migration policy.
Those remaining compatibility limits are why this payload is not described as a complete version-1 workspace envelope.

Changing a stable presentation token requires an explicit compatibility decision.
Built-in and custom presentation ids remain opaque references, while the exact field/sort/group/direction vocabulary is closed for a given presentation version.

## Example

This example contains one filtered All Tracks view and one custom presentation:

```yaml
workspace:
  presentationVersion: 1
  openViews:
    - listId: 4294967295
      filterExpression: '$genre = "Classical"'
      presentation:
        id: albums
        group: album
        sort:
          - field: album
            direction: ascending
          - field: title
            direction: descending
        visibleFields:
          - title
          - artist
          - duration
        redundantFields:
          - album
  activeListId: 4294967295
  customPresets:
    - label: Works
      basePresetId: library
      spec:
        id: custom.works
        group: work
        sort:
          - field: work
            direction: ascending
        visibleFields:
          - title
          - movement
        redundantFields:
          - work
```

Mapping order is not semantically significant, although canonical reflected emission follows DTO declaration order.

## Implementation authority

- [`WorkspaceSessionCodec.h`](../../../app/runtime/WorkspaceSessionCodec.h) owns exact stored DTO members and the presentation-version constant.
- [`WorkspaceSessionCodec.cpp`](../../../app/runtime/WorkspaceSessionCodec.cpp) owns stable conversion and semantic validation.
- [`WorkspaceSessionState.h`](../../../app/include/ao/rt/WorkspaceSessionState.h) owns the decoded semantic candidate.
- [`WorkspaceService.cpp`](../../../app/runtime/WorkspaceService.cpp) owns group selection, capture, candidate preparation, and installation.
- [`TrackField.h`](../../../app/include/ao/rt/TrackField.h) and [`TrackField.cpp`](../../../app/runtime/TrackField.cpp) own the stable vocabulary.

## Test authority

- [`WorkspaceSessionCodecTest.cpp`](../../../test/unit/runtime/WorkspaceSessionCodecTest.cpp) protects exact semantic conversion, canonical tokens, and invalid-object rejection.
- [`WorkspaceSessionTest.cpp`](../../../test/unit/runtime/WorkspaceSessionTest.cpp) protects missing, malformed, unsupported, strict-schema, fallback, and transactional restoration outcomes.
- [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) protects canonical persisted text and presentation reconstruction across runtime instances.
- [`TrackFieldTest.cpp`](../../../test/unit/runtime/TrackFieldTest.cpp) protects exhaustive stable token lookup.

## Related documents

- [Workspace architecture](../../architecture/workspace.md)
- [Workspace session specification](../../spec/workspace/session.md)
- [Workspace navigation specification](../../spec/workspace/navigation.md)
- [Persisted presentation state](../presentation/persisted-state.md)
- [Application managed-state surface](../persistence/application-config.md)
- [Grouped configuration store](../../spec/persistence/config-store.md)
- [Runtime track field catalog](../library/model/track-field.md)
- [Track presentation presets](../presentation/track-preset.md)
- [RFC 0017: versioned semantic workspace sessions](../../rfc/0017-versioned-workspace-session.md)
