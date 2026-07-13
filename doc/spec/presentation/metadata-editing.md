---
id: presentation.metadata-editing
type: spec
status: current
domain: presentation
summary: Defines track-detail aggregation, editable field policy, custom metadata and tag mutations, validation, and undo eligibility.
---
# Metadata-editing specification

## Scope

This specification owns frontend-neutral presentation and editing policy for built-in track metadata, technical properties, custom metadata, and tags across a selected track set.
It defines the runtime detail snapshot consumed by editors, UIModel field/schema policy, patch construction, multi-selection behavior, and mutation outcomes.

It does not define GTK geometry, chip widgets, popovers, or shell placement; those belong to the [GTK track-detail specification](../linux-gtk/track-detail.md).
It does not define tag-file import mappings or query grammar.

## Code boundary

Runtime `TrackDetailProjection` owns the authoritative aggregate snapshot and observes library/view changes.
`LibraryWriter` owns committed metadata/tag mutation and change publication.
UIModel code under `app/include/ao/uimodel/library/detail/`, `library/property/`, and `field/` owns schema, visibility, display formatting, validation, edit codecs, and patch/workflow construction.

UIModel may call the narrow runtime writer supplied by composition.
It does not open transactions or mutate `MusicLibrary` stores directly.
Frontends may render and collect edit intent but cannot reinterpret patch semantics.

## Terminology

- An **aggregate value** has an optional representative value plus a `mixed` flag.
- **Partial presence** means a custom key exists on at least one but not every selected track.
- A **built-in metadata field** is a system track field that is editable through a typed metadata patch.
- A **technical field** is an objective read-only property in the detail editor.
- A **common tag** is present on every selected track.
- An **undo-eligible deletion** removes a custom key that was present on all selected tracks with one non-mixed value.

## Invariants

- `TrackDetailSnapshot` contains one coherent selection kind, id set, revision, aggregate field array, custom metadata set, common-tag ids, and single-selection cover id.
- Synthetic display fields and tags are excluded from the built-in field grid; tags have their own editing surface.
- Technical fields are never editable through metadata UI policy.
- Mixed built-in/custom values display the shared `<Multiple Values>` marker, and that literal cannot be committed as a custom value.
- Updating a custom key applies the value to every selected target; deletion removes it from every selected target.
- A custom key cannot be added when already present in the snapshot or when it collides with a reserved built-in field id.
- Built-in metadata can be cleared but not structurally deleted.
- A tag edit with no selected ids or no additions/removals is a no-op.
- File tag readers map only explicitly supported Aobus fields; unknown vendor fields do not become custom metadata.

## State model

`TrackDetailSnapshot` uses `SelectionKind::{None, Single, Multiple}` and retains selected `TrackId` values.
Each built-in field is an `AggregateValue<TrackFieldRawValue>`.
Each `CustomMetadataItem` carries key, aggregate string value, `presentOnAll`, and `presentOnAny`.

The field-grid schema divides supported definitions into metadata, composite metadata, and technical fields according to the requested categories.
Visibility policy depends on category enablement, selection, section expansion, show-empty state, editor activity, and current display text.

## Commands and transitions

### Built-in metadata

The frontend decodes edit text through the shared field codec and creates a typed `MetadataPatch`.
Applying the patch through `LibraryWriter` updates every selected id that can be mutated and publishes the runtime library change.
An empty metadata display value remains hidden by default unless show-empty is active or its editor is open.

### Custom metadata

Addition first validates duplicate and reserved-key conflicts.
Update creates `customUpdates[key] = value`; delete creates `customUpdates[key] = nullopt`.
The mutation result's `mutatedIds` determines whether the visible operation applied.

Before deletion, UIModel returns an undo value only for a key present on all targets with a non-mixed value.
Presentation of and timeout for that undo are frontend-local, but replay uses the same runtime metadata patch.

### Tags

`TagEditWorkflow` sends selected ids plus additions/removals to `LibraryWriter::editTags`.
It reports whether any target changed, whether the request was rejected, and one status string summarizing added/removed counts and mutated track count.
Suggested tags are a presentation aid; only the final add/remove request is authoritative.

## Failure and cancellation

Runtime mutation failure rejects the edit and exposes the recoverable diagnostic to the frontend workflow.
No partial frontend state is treated as committed merely because an editor closed.
The current synchronous mutation boundary has no cancellation token; cancellation before submission discards the local draft, while a returned successful mutation is committed.
The current command carries no observed field baseline or library generation and silently skips missing target ids; [RFC 0023](../../rfc/0023-revision-bound-metadata-authoring.md) proposes guarded atomic authoring and compare-and-restore undo.

## Persistence and versioning

Built-in metadata, custom metadata, and tags persist in the library through the library mutation contract.
They are also represented by governed YAML transfer according to the [library YAML transfer specification](../library/runtime/yaml-transfer.md).
Editor visibility, expansion, drafts, and delete-undo state are not library data.

## Frontend observations

A frontend may distinguish no selection, mixed values, partial custom-key presence, empty metadata, and technical unknowns.
It may choose inline, form, or command interaction while using the same schema, codec, validation, and runtime writer authority.

Custom keys are queryable through the custom-variable syntax in the predicate language; presentation does not reinterpret or restrict that grammar.

## Implementation map

- [`TrackDetailProjection.h`](../../../app/include/ao/rt/projection/TrackDetailProjection.h) defines the aggregate snapshot.
- [`LiveTrackDetailProjection.cpp`](../../../app/runtime/projection/LiveTrackDetailProjection.cpp) builds and observes live snapshots.
- [`TrackFieldGridSchema.cpp`](../../../app/uimodel/library/detail/TrackFieldGridSchema.cpp) and [`TrackFieldGridPolicy.h`](../../../app/include/ao/uimodel/library/detail/TrackFieldGridPolicy.h) own field selection and visibility.
- [`TrackCustomMetadataWorkflow.cpp`](../../../app/uimodel/library/detail/TrackCustomMetadataWorkflow.cpp) owns display, validation, patches, and undo eligibility.
- [`TagEditWorkflow.cpp`](../../../app/uimodel/library/property/TagEditWorkflow.cpp) owns tag mutation requests and status.
- [`LibraryWriter.cpp`](../../../app/runtime/library/LibraryWriter.cpp) owns mutation commit.

## Test map

- Runtime projection tests under [`test/unit/runtime/projection/`](../../../test/unit/runtime/projection/) protect aggregation and refresh.
- [`TrackFieldGridSchemaTest.cpp`](../../../test/unit/uimodel/library/detail/TrackFieldGridSchemaTest.cpp) and [`TrackFieldGridPolicyTest.cpp`](../../../test/unit/uimodel/library/detail/TrackFieldGridPolicyTest.cpp) protect field/visibility policy.
- [`TrackCustomMetadataWorkflowTest.cpp`](../../../test/unit/uimodel/library/detail/TrackCustomMetadataWorkflowTest.cpp) protects validation, patches, mixed values, and undo eligibility.
- [`TagEditWorkflowTest.cpp`](../../../test/unit/uimodel/library/property/TagEditWorkflowTest.cpp) protects tag mutations and outcomes.
- [`LibraryWriterTest.cpp`](../../../test/unit/runtime/library/LibraryWriterTest.cpp) protects committed multi-target behavior.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Library architecture](../../architecture/library.md)
- [Track model reference](../../reference/library/model/track.md)
- [Track field reference](../../reference/library/model/track-field.md)
- [Predicate language reference](../../reference/query/predicate-language.md)
- [GTK track-detail specification](../linux-gtk/track-detail.md)
- [RFC 0023: revision-bound metadata authoring](../../rfc/0023-revision-bound-metadata-authoring.md)
