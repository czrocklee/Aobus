---
id: workspace.session-state
---

# Workspace session state

## Scope and version

This reference owns the exact current payload of the `workspace` configuration group.
It enumerates the private persistence document, nested view presentations and custom presets, stable values, strict validation, and compatibility behavior.

The required `presentationVersion` is currently `3`.
It versions the nested presentation vocabulary, not the complete workspace document.
The payload has no root workspace schema version or resource budget.

The [workspace session specification](../../system/workspace/session.md) owns capture, candidate creation, exact focus selection, commit, and failures after deserialization.
The [application managed-state surface](../persistence/application-config.md) owns document and writer registration.

## Code boundary

This surface belongs to the application runtime layer from the [system architecture](../../system/overview.md), as refined by the [workspace architecture](../../system/workspace/README.md) and [persistence and managed-state architecture](../../system/persistence/README.md).
`WorkspaceSessionState` remains the runtime semantic candidate.
Its `activeViewIndex` is a `std::size_t`; the private persistence DTO stores the field as `std::uint32_t`.
The private `WorkspaceSessionDocument` and nested stored DTOs in `app/runtime/WorkspaceSessionYamlSchema.h` are the exact YAML model.
`WorkspaceSessionYamlSchema.cpp` converts between that document and runtime `TrackListViewConfig`, `TrackPresentationSpec`, and custom-preset values.

`WorkspaceService` selects the literal `workspace` group and passes `WorkspaceSessionYamlSchema` explicitly to `ConfigStore::load()` and `save()`.
The schema performs structural deserialization into its private persistence document, semantic conversion into `WorkspaceSessionState`, and candidate validation before the store can assign the result.
Frontends select the containing path but do not deserialize or redefine fields.

## Surface

### Group root

The literal top-level group is `workspace`.
Its value is one mapping with these required fields in canonical emitted order:

| Field                 | YAML shape                          | Meaning                                                                             |
| --------------------- | ----------------------------------- | ----------------------------------------------------------------------------------- |
| `presentationVersion` | Unsigned 32-bit scalar.             | Required and exactly `3`.                                                           |
| `libraryId`           | Canonical UUID string.              | Library the session was saved for; its `openViews` list ids belong to that library. |
| `openViews`           | Sequence of view mappings.          | Ordered semantic view reconstruction records.                                       |
| `activeViewIndex`     | Unsigned 32-bit scalar.             | Zero-based position of the exact active entry in `openViews`.                       |
| `customPresets`       | Sequence of custom-preset mappings. | Complete workspace custom-preset collection.                                        |

The session stores no `ViewId`.
`activeViewIndex` identifies an ordered semantic entry, so multiple filtered or differently presented views over one base list remain unambiguous across runtime lifetimes.

### Track-list view entry

Every `openViews` entry contains:

| Field              | YAML shape              | Requirement and meaning                                            |
| ------------------ | ----------------------- | ------------------------------------------------------------------ |
| `listId`           | Unsigned 32-bit scalar. | Required and nonzero; identifies the base library or virtual list. |
| `filterExpression` | String.                 | Required; empty means no transient filter.                         |
| `presentation`     | Presentation mapping.   | Required exact presentation used to reconstruct the view.          |

Grouping and sorting occur only inside the required exact presentation.

### Presentation mapping

Each view `presentation` and custom-preset `spec` contains:

| Field             | YAML shape                              | Requirement                                                                                              |
| ----------------- | --------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `kind`            | `builtin` or `custom`.                  | Required. A `customPresets` entry's `spec` must use `custom`.                                            |
| `presetId`        | String.                                 | Required. Built-in slug for `builtin`; canonical custom key for `custom`.                                |
| `group`           | Stable group-key string.                | Required and known.                                                                                      |
| `sort`            | Sequence of sort-term mappings.         | Required base order. May be empty. Sort fields must be known and unique within this sequence.            |
| `columnSort`      | One column-sort mapping.                | Optional on a view. Omitted when absent. `column` is a track-field id. Forbidden in a custom definition. |
| `visibleFields`   | Sequence of stable track-field strings. | Required, nonempty, known, and unique.                                                                   |
| `redundantFields` | Sequence of stable track-field strings. | Required, known, and unique.                                                                             |

The [preset reference](../presentation/track-preset.md) defines the closed built-in slugs and the custom key format: exactly 16 lowercase hexadecimal digits. Deserialization checks that a key matches its `kind`. It does not require a custom id to appear in `customPresets`, because the view stores its complete presentation.
The field and group token catalogs are owned by the [runtime track-field catalog](../library/model/track-field.md) and routed through the [persisted presentation-state reference](../presentation/persisted-state.md).

### Sort term

Every `sort` entry contains:

| Field       | YAML shape                | Requirement                                    |
| ----------- | ------------------------- | ---------------------------------------------- |
| `field`     | Stable sort-field string. | Required, known, and unique within `sort`.     |
| `direction` | String.                   | Required; exactly `ascending` or `descending`. |

`columnSort`, when present, is one mapping and not a sequence:

| Field       | YAML shape             | Requirement                                         |
| ----------- | ---------------------- | --------------------------------------------------- |
| `column`    | Stable track-field id. | Required, known, and sortable. Not a sort-field id. |
| `direction` | String.                | Required; exactly `ascending` or `descending`.      |

`field` is not accepted in `columnSort`, and `column` is not accepted in a `sort` term.

### Custom presentation preset

Every `customPresets` entry contains:

| Field          | YAML shape            | Meaning                                                    |
| -------------- | --------------------- | ---------------------------------------------------------- |
| `label`        | String.               | User-visible label.                                        |
| `basePresetId` | String.               | Built-in slug, canonical custom key, or empty for no base. |
| `spec`         | Presentation mapping. | Complete spec with `kind: custom` and no `columnSort`.     |

## Validation rules

- The explicit schema validates each mapping and sequence recursively. `presetId` must match `kind`. `columnSort` does not select a kind.
- Unknown members, missing members, wrong node kinds, malformed scalar values, and malformed vector elements reject the whole workspace document.
- `presentationVersion` must be `3`.
- `libraryId` must be a canonical 8-4-4-4-12 hexadecimal UUID; whether it matches the open library is decided at restore, not deserialization.
- When `openViews` is empty, `activeViewIndex` must be `0`.
- When `openViews` is nonempty, `activeViewIndex` must be smaller than `openViews.size()`.
- Deserialization violations of either active-index rule return `FormatRejected` before candidate-view creation.
- View list ids must be nonzero; existence in the active library is checked while candidate views are created.
- Closed kind, column, field, sort, group, and direction tokens must resolve exactly and case-sensitively.
- Duplicate fields inside `sort`, and duplicate values within either field collection, reject the document. `columnSort` stores the clicked track-field id, not its projected sort key, and is not expanded into disc and track terms.
- A present `columnSort` must be one mapping with exactly `column` and `direction`. A sequence, a missing member, an unknown or unsortable column, an unknown direction, an extra member, or a `field` member rejects the document. `kind: columnSort` is not a kind.
- A `customPresets` spec that contains `columnSort` rejects the document. A view presentation of either kind may contain it.
- Invalid preset keys and mismatches between `kind` and `presetId` reject the document. Deserialized presentations must have at least one visible field.
- Serialization has caller preconditions: the semantic active-view index must fit the unsigned 32-bit DTO and satisfy the empty/nonempty bounds; every view must have a nonzero list id and an exact presentation with valid enum values and a preset id. A custom-preset definition must not carry `optColumnSort`. These are live workspace invariants, and violating them is a contract failure rather than a recoverable `InvalidState` result.
- Before emission, serialization normalizes permitted presentation defaults, deduplicates `visibleFields` and `redundantFields`, and supplies `title` for an empty visible set. It does not deduplicate `sortBy`: duplicate sort fields violate a serialization precondition enforced by `AO_EXPECTS`. It emits `columnSort` only when an override is present, after `sort` and before `visibleFields`, as one `column`/`direction` mapping. `column` is the track-field id, such as `display-track-number`, not a sort-field id and not an expanded disc-plus-track sequence. `sort` continues to use `field`. Canonical output therefore has unique base sort and field entries and a nonempty `visibleFields` sequence.
- Structural and semantic deserialize completes before `WorkspaceService` creates or installs candidate views.

There is no schema-level limit for view count, preset count, string length, sort-term count, or field count.
The schema does not check custom ids for uniqueness. Built-in and custom ids are distinct types and cannot collide.

## Compatibility and versioning

The reader accepts only version 3. That vocabulary has not been published. It rejects version 2, version 1, unversioned state, numeric enums, `kind: columnSort`, and unknown closed tokens without migration or automatic rewrite. `field` is not an alias for `columnSort.column`.
An unsupported version returns `NotSupported` before version-specific sibling fields are interpreted. The old `id` field is not accepted as an alias for `kind` or `presetId`.

Strict deserialization requires the exact root field set; it does not parse, migrate, or emit alternate root names.
`presentationVersion` promises only the nested presentation vocabulary, not a complete root version or collection budgets.

Changing a stable presentation token requires an explicit compatibility decision.
The kind, built-in slug, field, sort, group, and direction vocabularies are closed for a given presentation version. The custom key format encodes a `uint64_t`, not an extensible name.

## Example

This example contains one filtered All Tracks view with a descending `display-track-number` header override and one named custom preset:

```yaml
workspace:
  presentationVersion: 3
  libraryId: 123e4567-e89b-12d3-a456-426614174000
  openViews:
    - listId: 4294967295
      filterExpression: '$genre = "Classical"'
      presentation:
        kind: builtin
        presetId: albums
        group: album
        sort:
          - field: album
            direction: ascending
          - field: title
            direction: ascending
        columnSort:
          column: display-track-number
          direction: descending
        visibleFields:
          - title
          - artist
          - duration
        redundantFields:
          - album
  activeViewIndex: 0
  customPresets:
    - label: Works
      basePresetId: library
      spec:
        kind: custom
        presetId: 00000000000000ab
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

Mapping order is not semantically significant; canonical explicit emission follows the field order shown above.

## Implementation authority

- [`WorkspaceSessionYamlSchema.h`](../../../app/runtime/WorkspaceSessionYamlSchema.h) owns exact stored DTO members, the presentation-version constant, and the explicit store schema.
- [`WorkspaceSessionYamlSchema.cpp`](../../../app/runtime/WorkspaceSessionYamlSchema.cpp) owns structural YAML mapping, stable conversion, and semantic validation.
- [`WorkspaceSessionState.h`](../../../app/include/ao/rt/WorkspaceSessionState.h) owns the deserialized semantic candidate.
- [`WorkspaceService.cpp`](../../../app/runtime/WorkspaceService.cpp) owns group selection, capture, candidate preparation, and installation.
- [`TrackField.h`](../../../app/include/ao/rt/TrackField.h) and [`TrackField.cpp`](../../../app/runtime/TrackField.cpp) own the stable vocabulary.

## Test authority

- [`WorkspaceSessionYamlSchemaTest.cpp`](../../../test/runtime/WorkspaceSessionYamlSchemaTest.cpp) protects exact semantic conversion, canonical root emission, active-index bounds and representation, stable tokens, and invalid-object rejection.
- [`WorkspaceSessionTest.cpp`](../../../test/runtime/WorkspaceSessionTest.cpp) protects missing, malformed, unsupported, strict-schema, exact-focus, bounds, empty-candidate, and transactional restoration outcomes.
- [`HeadlessShellTest.cpp`](../../../test/runtime/HeadlessShellTest.cpp) protects canonical persisted text and presentation reconstruction across runtime instances.
- [`TrackFieldTest.cpp`](../../../test/runtime/TrackFieldTest.cpp) protects exhaustive stable token lookup.

## Related documents

- [Workspace architecture](../../system/workspace/README.md)
- [Workspace session specification](../../system/workspace/session.md)
- [Workspace navigation specification](../../system/workspace/navigation.md)
- [Persisted presentation state](../presentation/persisted-state.md)
- [Application managed-state surface](../persistence/application-config.md)
- [Grouped configuration store](../../system/persistence/config-store.md)
- [Runtime track field catalog](../library/model/track-field.md)
- [Track presentation presets](../presentation/track-preset.md)
