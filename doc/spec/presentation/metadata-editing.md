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
It defines the runtime detail snapshot consumed by editors, UIModel field/schema policy, patch construction, multi-selection behavior, and mutation results.

It does not define GTK geometry, chip widgets, popovers, or shell placement; those belong to the [GTK track-detail specification](../linux-gtk/track-detail.md).
It does not define tag-file import mappings or query grammar.

## Code boundary

Runtime `TrackDetailProjection` owns the authoritative aggregate snapshot and observes library/view changes.
`LibraryMutationService` owns admission, commit, and change publication; `LibraryWriter` exposes bound metadata/tag commands.
UIModel code under `app/include/ao/uimodel/library/detail/`, `library/property/`, and `field/` owns schema, visibility, display formatting, validation, edit codecs, and patch construction.

`TrackAuthoringSession` is the UIModel boundary for committing metadata and tag edits and may call the narrow runtime writer supplied by composition.
It does not open transactions or mutate `MusicLibrary` stores directly.
Interactive GTK/TUI frontends may render and collect edit intent but cannot call `LibraryWriter` directly, replace the bound targets, or reinterpret patch semantics.
The non-interactive CLI may bind command-selected ids immediately before invoking the runtime writer, as defined by the [CLI execution specification](../cli/execution.md).

## Terminology

- An **aggregate value** has an optional representative value plus a `mixed` flag.
- **Partial presence** means a custom key exists on at least one but not every selected track.
- A **built-in metadata field** is a system track field that is editable through a typed metadata patch.
- A **technical field** is an objective read-only property in the detail editor.
- A **common tag** is present on every selected track.
- An **undo-eligible deletion** removes a custom key that was present on all selected tracks with one non-mixed value.
- An **authoring binding** identifies one runtime instance, one committed library revision, and one exact ordered target-id set.

## Invariants

- `TrackDetailSnapshot` contains one coherent selection kind, id set, aggregate field array, custom metadata set, common-tag ids, and single-selection cover id.
- Synthetic display fields and tags are excluded from the built-in field grid; tags have their own editing surface.
- Technical fields are never editable through metadata UI policy.
- Mixed built-in/custom values display the shared `<Multiple Values>` marker, and that literal cannot be committed as a custom value.
- Updating a custom key applies the value to every selected target; deletion removes it from every selected target.
- A custom key cannot be added when already present in the snapshot or when it collides with a reserved built-in field id.
- Built-in metadata can be cleared but not structurally deleted.
- A tag edit with no selected ids or no additions/removals is a no-op.
- One open editor owns one `TrackAuthoringSession`; changing selection or recycling a row cannot retarget that session.
- Any intervening effective library commit, maintenance entry, fault, or runtime replacement invalidates an open session.
- Missing targets reject the complete metadata/tag command; multi-selection authoring never applies a surviving subset.
- A semantic no-op does not commit and leaves the current session binding usable.
- File tag readers map only explicitly supported Aobus fields; unknown vendor fields do not become custom metadata.

## State model

`TrackDetailSnapshot` uses `SelectionKind::{None, Single, Multiple}` and retains selected `TrackId` values.
Each built-in field is an `AggregateValue<TrackFieldRawValue>`.
Each `CustomMetadataItem` carries key, aggregate string value, `presentOnAll`, and `presentOnAny`.

The field-grid schema divides supported definitions into metadata, composite metadata, and technical fields according to the requested categories.
Visibility policy depends on category enablement, selection, section expansion, show-empty state, editor activity, and current display text.

`TrackAuthoringSession` exposes only whether its retained binding is current and a one-shot invalidation observation.
It has no public submission lifecycle: submission is synchronous, and frontends only need to know whether another command may use the same binding.
Beginning a session binds its explicit targets and immediately reconciles current runtime availability after subscribing, closing the bind-to-subscribe event gap.
During its own submission, the session defers availability invalidation until the runtime result supplies the next binding.
An applied submission replaces the retained binding with that next-revision binding; a later effective commit invalidates it.
Operational failure, a missing target, stale or unavailable status, or mismatched post-submit availability also invalidates it.
A durably applied command whose publication exception is contained by the callback executor therefore retains its `Applied` result but leaves the session invalid because that runtime is faulted.

## Commands and transitions

### Built-in metadata

The frontend decodes edit text through the shared field codec and creates a typed `MetadataPatch`.
Applying the patch through the retained authoring session updates the complete bound target set or none of it.
The result is `Applied`, `NoOp`, `Stale`, `Missing`, or `Unavailable`; `Result` errors remain operational or validation failures.
An empty metadata display value remains hidden by default unless show-empty is active or its editor is open.

### Custom metadata

Addition first validates duplicate and reserved-key conflicts.
Update creates `customUpdates[key] = value`; delete creates `customUpdates[key] = nullopt`.
An empty `changes` list is a no-op; each applied entry identifies its track and changed fields or tags.

Before deletion, UIModel returns an undo value only for a key present on all targets with a non-mixed value.
An applied deletion transfers its session, now holding the next-revision binding, into the pending undo state.
Presentation and timeout remain frontend-local, but replay submits the reverse patch through that same guarded session.
Any intervening effective commit makes undo stale instead of overwriting newer work.

### Tags

`applyTagEdit()` submits additions/removals through the targets already bound to its `TrackAuthoringSession`; it does not copy or rebind a second selected-id set.
Its result reuses `TrackAuthoringStatus` and carries display text only when the frontend has something to report.
Suggested tags are a presentation aid; only the final add/remove command is authoritative.

## Failure and cancellation

Runtime mutation failure rejects the edit and exposes the recoverable diagnostic to the frontend workflow.
No partial frontend state is treated as committed merely because an editor closed.
GTK table inline edits place parsing, operational, stale, missing, and unavailable failures in the table's existing status surface and update the row only after `Applied`.
GTK detail-grid parsing and submission failures create an error notification and restore the pre-edit display value; the backing library remains unchanged on rejection.
Custom-metadata undo returns its terminal failure, clears the expired action, and the undo bar publishes that failure as an error notification.
The current synchronous mutation boundary has no cancellation token; cancellation before submission discards the local draft, while a returned successful mutation is committed.

Stale and unavailable outcomes tell the frontend to reload rather than retry the same session.
Missing targets reject the session and report the missing-target condition.
After durable commit, a publication failure faults the runtime; UIModel cannot treat it as an ordinary uncommitted rejection.
It preserves an applied outcome when available while making the session stale, so the frontend can show committed state but cannot submit through that runtime again.

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
- [`TrackCustomMetadata.cpp`](../../../app/uimodel/library/detail/TrackCustomMetadata.cpp) owns display, validation, patches, and undo eligibility.
- [`TagEdit.cpp`](../../../app/uimodel/library/property/TagEdit.cpp) owns tag mutation submission and status text.
- [`TrackAuthoringSession.h`](../../../app/include/ao/uimodel/library/property/TrackAuthoringSession.h) owns stable targets, current-binding lifetime, invalidation, and result mapping.
- [`LibraryWriter.cpp`](../../../app/runtime/library/LibraryWriter.cpp) owns mutation commit.

## Test map

- Runtime projection tests under [`test/unit/runtime/projection/`](../../../test/unit/runtime/projection/) protect aggregation and refresh.
- [`TrackFieldGridSchemaTest.cpp`](../../../test/unit/uimodel/library/detail/TrackFieldGridSchemaTest.cpp) and [`TrackFieldGridPolicyTest.cpp`](../../../test/unit/uimodel/library/detail/TrackFieldGridPolicyTest.cpp) protect field/visibility policy.
- [`TrackCustomMetadataTest.cpp`](../../../test/unit/uimodel/library/detail/TrackCustomMetadataTest.cpp) protects validation, patches, mixed values, and undo eligibility.
- [`TagEditTest.cpp`](../../../test/unit/uimodel/library/property/TagEditTest.cpp) protects tag mutations and statuses.
- [`TrackAuthoringSessionTest.cpp`](../../../test/unit/uimodel/library/property/TrackAuthoringSessionTest.cpp) protects stable target order, no-op reuse, invalidation, and post-commit faults with both propagating and exception-containing executors.
- [`LibraryWriterTest.cpp`](../../../test/unit/runtime/library/LibraryWriterTest.cpp) protects committed multi-target behavior.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Library architecture](../../architecture/library.md)
- [Track model reference](../../reference/library/model/track.md)
- [Track field reference](../../reference/library/model/track-field.md)
- [Predicate language reference](../../reference/query/predicate-language.md)
- [GTK track-detail specification](../linux-gtk/track-detail.md)
