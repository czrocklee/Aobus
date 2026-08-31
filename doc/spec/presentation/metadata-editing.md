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
`LibraryWriteLane` owns admission, commit, and change publication; `LibraryCommands` exposes bound metadata/tag commands.
UIModel code under `app/include/ao/uimodel/library/detail/`, `library/property/`, `library/track/`, and `field/` owns schema, visibility, display formatting, validation, edit decoding, and patch construction.

`TrackAuthoringSession` is the UIModel boundary for committing metadata and tag edits and may call the bound runtime commands supplied by composition.
It does not open transactions or mutate `MusicLibrary` stores directly.
Interactive GTK, WinUI, and TUI frontends may render and collect edit intent but cannot call `LibraryCommands` directly, replace the bound targets, or reinterpret patch semantics.
The non-interactive CLI may bind command-selected ids immediately before invoking the runtime commands, as defined by the [CLI execution specification](../cli/execution.md).

## Terminology

- An **aggregate value** has an optional representative value plus a `mixed` flag.
- **Partial presence** means a custom key exists on at least one but not every selected track.
- A **built-in metadata field** is a system track field that is editable through a typed metadata patch.
- A **technical field** is an objective read-only property in the detail editor.
- A **common tag** is present on every selected track.
- An **undo-eligible deletion** removes a custom key that was present on all selected tracks with one non-mixed value.
- An **authoring binding** identifies one runtime instance, one committed library revision, and one exact ordered target-id set.
- **Session State** is the independently retained asynchronous binding/invalidation state behind a move-only authoring facade; it borrows the runtime `Library`.

## Invariants

- `TrackDetailSnapshot` contains one coherent selection kind, id set, aggregate field array, custom metadata set, common-tag ids, and single-selection cover id.
- Synthetic display fields and tags are excluded from the built-in field grid; tags have their own editing surface.
- Technical fields are never editable through metadata UI policy.
- Mixed built-in/custom values display the shared `<Multiple Values>` marker, and that literal cannot be committed as a custom value.
- Updating a custom key applies the value to every selected target; deletion removes it from every selected target.
- A custom key cannot be added when already present in the snapshot or when it collides with a reserved built-in field id.
- Built-in metadata can be cleared but not structurally deleted.
- A tag edit with no selected ids or no additions/removals is a no-op.
- One open editor owns one move-only `TrackAuthoringSession` value; changing selection or recycling a row cannot retarget that session.
- A submitted operation retains Session State and may settle after the public facade is moved or destroyed, but it cannot outlive the borrowed runtime `Library`.
- A WinUI window admits at most one track-properties dialog, and that dialog retains the selection captured when it opened.
- Any intervening effective library commit, maintenance entry, fault, or runtime replacement invalidates an open session.
- Missing targets reject the complete metadata/tag command; multi-selection authoring never applies a surviving subset.
- A Properties save submits its metadata and tag intent as one runtime command; rejection cannot commit only one part.
- A semantic no-op does not commit and leaves the current session binding usable.
- File tag readers map only explicitly supported Aobus fields; unknown vendor fields do not become custom metadata.

## State model

`TrackDetailSnapshot` uses `SelectionKind::{None, Single, Multiple}` and retains selected `TrackId` values.
Each built-in field is an `AggregateValue<TrackFieldRawValue>`.
Each `CustomMetadataItem` carries key, aggregate string value, `presentOnAll`, and `presentOnAny`.

The field-grid schema divides supported definitions into metadata, composite metadata, and technical fields according to the requested categories.
Visibility policy depends on category enablement, selection, section expansion, show-empty state, editor activity, and current display text.

`TrackAuthoringSession::begin()` returns a move-only value facade that exposes only whether its retained binding is current and a one-shot invalidation observation.
The facade holds shared Session State because a submitted coroutine independently retains binding, subscription, and one-pending-command state through completion; this is not shared ownership of the public facade or runtime.
Submission is asynchronous, and one State admits at most one pending command; another submission receives non-terminal `Busy` without replacing the retained draft or binding.
Beginning a session binds its explicit targets and immediately reconciles current runtime availability after subscribing, closing the bind-to-subscribe event gap.
During its own submission, the session defers availability invalidation until the runtime result supplies the next binding.
An applied submission replaces the retained binding with that next-revision binding; a later effective commit invalidates it.
Operational failure, stale or unavailable status, maintenance observed during submission, or mismatched post-submit availability also invalidates it.

## Commands and transitions

### Built-in metadata

The frontend decodes edit text through the shared field codec and creates a typed `MetadataPatch`.
Applying the patch through the retained authoring session updates the complete bound target set or none of it.
The result is `Applied`, `NoOp`, `Busy`, `Stale`, or `Unavailable`; `Result` errors remain operational or validation failures.
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
Its result reuses `AuthoringStatus` and carries display text only when the frontend has something to report.
Suggested tags are a presentation aid; only the final add/remove command is authoritative.

GTK adapts the shared tag-frequency result before applying its visible limit:
frequency remains descending, equal-frequency values use the startup-locale
ordering key, and raw NFC bytes break equal locale keys. The shared library
reader remains byte ordered because the CLI consumes that same storage-facing
result without an interactive locale.

The tag editor's writable saved-List chooser orders eligible List names by the
interactive locale key and uses `ListId` for equal keys. This changes only the
chooser presentation; eligibility, List identity, tag expression, and the
submitted membership command are unchanged. Without an interactive ordering
policy, the UIModel helper retains name-byte order followed by `ListId`.

### Combined Properties save

A form that collects metadata and tags submits one `TrackPropertiesPatch` through its retained authoring session.
Runtime applies both parts in one write transaction and publishes at most one revision and changeset.
The form closes only after that command returns `Applied` or `NoOp`; `Busy`, stale/unavailable state, and `Result` errors leave the form open or disabled according to the session state without representing either part as committed.

## Failure and cancellation

Runtime mutation failure rejects the edit and exposes the recoverable diagnostic to the frontend workflow.
For a combined Properties save, failure after either part has staged changes aborts the complete write transaction, so reusing the unchanged form draft cannot resubmit an already committed metadata half.
No partial frontend state is treated as committed merely because an editor closed.
GTK table inline edits place parsing, operational, stale, and unavailable failures in the table's existing status surface and update the row only after `Applied`.
GTK detail-grid parsing and submission failures create an error notification and restore the pre-edit display value; the backing library remains unchanged on rejection.
Custom-metadata undo returns its terminal failure, clears the expired action, and the undo bar publishes that failure as an error notification.
The current synchronous mutation boundary has no cancellation token; cancellation before submission discards the local draft, while a returned successful mutation is committed.
Destroying or moving the facade after submission does not itself cancel the command, because the coroutine retains Session State; the owning frontend workflow must settle that task before destroying the runtime `Library` borrowed by the State.

Stale and unavailable outcomes tell the frontend to reload rather than retry the same session.
Missing targets are rejected with `NotFound` while creating the binding.
Once an exact-revision binding exists, target disappearance without a newer committed revision is an invariant violation rather than a frontend-recoverable status.
After durable commit, a publication failure faults the runtime; UIModel cannot treat it as an ordinary uncommitted rejection.
The authoring session invalidates itself and propagates that exception to its caller; it does not translate the failure into an `Applied` result.

## Persistence and versioning

Built-in metadata, custom metadata, and tags persist in the library through the library mutation contract.
They are also represented by governed YAML transfer according to the [library YAML transfer specification](../library/runtime/yaml-transfer.md).
Editor visibility, expansion, drafts, and delete-undo state are not library data.

## Frontend observations

A frontend may distinguish no selection, mixed values, partial custom-key presence, empty metadata, and technical unknowns.
It may choose inline, form, or command interaction while using the same schema, codec, validation, and runtime writer authority.

WinUI presents one native `ContentDialog` for a captured single- or multi-track selection.
The dialog exposes editable built-in metadata, tags common to every target, custom metadata, and read-only technical properties.
It opens from the row context menu, an ordinary shell menu, or the window-local `Alt+Enter` accelerator; right-clicking an unselected row selects that row before opening its menu.
Save is disabled while the draft is unchanged or invalid, remains open across `Busy` and recoverable failures, and closes only after the combined Properties submission is accepted.
A stale session disables submission and asks the user to reopen the dialog from the current selection.

Custom keys are queryable through the custom-variable syntax in the predicate language; presentation does not reinterpret or restrict that grammar.
Locale ordering affects only suggestion and chooser position. It never changes
tag equality, matching, stored tag bytes, or mutation semantics.

## Implementation map

- [`TrackDetailProjection.h`](../../../app/include/ao/rt/projection/TrackDetailProjection.h) defines the aggregate snapshot.
- [`TrackDetailProjection.cpp`](../../../app/runtime/projection/TrackDetailProjection.cpp) builds and observes live snapshots.
- [`TrackFieldGrid.cpp`](../../../app/uimodel/library/detail/TrackFieldGrid.cpp) and [`TrackFieldGrid.h`](../../../app/include/ao/uimodel/library/detail/TrackFieldGrid.h) own field selection and visibility.
- [`TrackAuthoring.h`](../../../app/include/ao/uimodel/library/track/TrackAuthoring.h) owns edit decoding, writable-field classification, patch construction, and inline mixed-value protection.
- [`TrackPropertiesFormModel.h`](../../../app/include/ao/uimodel/library/property/TrackPropertiesFormModel.h) and [`TrackPropertiesFormSpec.h`](../../../app/include/ao/uimodel/library/property/TrackPropertiesFormSpec.h) own compact form state, mixed-value policy, editor kinds, and patch construction.
- [`TrackCustomMetadata.cpp`](../../../app/uimodel/library/detail/TrackCustomMetadata.cpp) owns display, validation, patches, and undo eligibility.
- [`TagEdit.cpp`](../../../app/uimodel/library/property/TagEdit.cpp) owns tag mutation submission and status text.
- [`TrackAuthoringSessions.h`](../../../app/include/ao/uimodel/library/track/TrackAuthoringSessions.h) owns the move-only value facade and stable targets; [`TrackAuthoringSession.cpp`](../../../app/uimodel/library/track/TrackAuthoringSession.cpp) owns shared asynchronous State, current-binding lifetime, invalidation, and result mapping.
- [`LibraryCommands.cpp`](../../../app/runtime/library/LibraryCommands.cpp) owns mutation commit.
- [`TrackPropertiesCoordinator`](../../../app/windows-winui/track/TrackPropertiesCoordinator.h) owns the native dialog and guarded asynchronous workflow; [`TrackPropertiesAdapter`](../../../app/windows-winui/include/ao/winui/track/TrackPropertiesAdapter.h) maps shared form and vocabulary state without WinRT.

## Test map

- Runtime projection tests under [`test/unit/runtime/projection/`](../../../test/unit/runtime/projection/) protect aggregation and refresh.
- [`TrackFieldGridSchemaTest.cpp`](../../../test/unit/uimodel/library/detail/TrackFieldGridSchemaTest.cpp) and [`TrackFieldGridVisibilityTest.cpp`](../../../test/unit/uimodel/library/detail/TrackFieldGridVisibilityTest.cpp) protect field/visibility policy.
- [`TrackAuthoringTest.cpp`](../../../test/unit/uimodel/library/track/TrackAuthoringTest.cpp) protects edit decoding, writable-field coverage, patch construction, and mixed-value sentinels.
- [`TrackCustomMetadataTest.cpp`](../../../test/unit/uimodel/library/detail/TrackCustomMetadataTest.cpp) protects validation, patches, mixed values, and undo eligibility.
- [`TagEditTest.cpp`](../../../test/unit/uimodel/library/property/TagEditTest.cpp) protects tag mutations and statuses.
- [`TrackAuthoringSessionTest.cpp`](../../../test/unit/uimodel/library/track/TrackAuthoringSessionTest.cpp) protects stable target order, no-op reuse, successful binding advancement, invalidation after another commit, move-only facade semantics, and a pending submission settling after moved and destroyed facades.
- [`LibraryCommandsTest.cpp`](../../../test/unit/runtime/library/LibraryCommandsTest.cpp) protects committed multi-target behavior.
- [`LibraryCommandsTrackPropertiesTest.cpp`](../../../test/unit/runtime/library/LibraryCommandsTrackPropertiesTest.cpp) protects combined metadata/tag publication and rollback when the later tag stage fails.
- [`TrackPropertiesAdapterTest.cpp`](../../../test/unit/winui/track/TrackPropertiesAdapterTest.cpp) protects WinUI control projection, mixed values, edit parsing, command availability, commit-state mapping, and tag/custom-key completion without WinRT.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Library architecture](../../architecture/library.md)
- [Track model reference](../../reference/library/model/track.md)
- [Track field reference](../../reference/library/model/track-field.md)
- [Predicate language reference](../../reference/query/predicate-language.md)
- [GTK track-detail specification](../linux-gtk/track-detail.md)
