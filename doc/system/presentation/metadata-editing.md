---
id: presentation.metadata-editing
---
# Metadata-editing specification

## Scope

This specification owns frontend-neutral presentation and editing policy for built-in track metadata, technical properties, custom metadata, and tags across a selected track set.
It defines the runtime detail snapshot consumed by editors, UIModel field/schema policy, patch construction, multi-selection behavior, and mutation results.

It does not define GTK geometry, chip widgets, popovers, or shell placement; those belong to the [GTK track-detail specification](../frontend/gtk/track-detail.md).
It does not define tag-file import mappings or query grammar.

## Code boundary

Runtime `TrackDetailProjection` owns the authoritative aggregate snapshot and observes library/view changes.
`LibraryWriteLane` owns admission, commit, and change publication; `LibraryCommands` exposes bound metadata/tag commands.
UIModel code under `app/include/ao/uimodel/library/detail/`, `library/property/`, `library/track/`, and `field/` owns schema, visibility, display formatting, validation, edit decoding, and patch construction.

`TrackAuthoringSession` is the UIModel boundary for committing metadata and tag edits and may call the bound runtime commands supplied by composition.
It does not open transactions or mutate `MusicLibrary` stores directly.
Interactive GTK, WinUI, TUI, and AppKit frontends may render and collect edit intent but cannot call `LibraryCommands` directly, replace the bound targets, or reinterpret patch semantics.
The non-interactive CLI may bind command-selected ids immediately before invoking the runtime commands, as defined by the [CLI execution specification](../frontend/cli.md).

## Terminology

- An **aggregate value** has an optional representative value plus a `mixed` flag.
- **Partial presence** means a custom key exists on at least one but not every selected track.
- A **built-in metadata field** is a system track field that is editable through a typed metadata patch.
- A **technical field** is an objective read-only property in the detail editor.
- A **common tag** is present on every selected track.
- An **undo-eligible deletion** removes a custom key present on every selected track with one common stored value, including a present empty value. Mixed or missing values are not undo-eligible. UIModel does not return this prior value.
- An **authoring binding** identifies one runtime instance, one committed library revision, and one exact ordered target-id set.
- **Session State** is the independently retained asynchronous binding/invalidation state behind a move-only authoring facade; it borrows the runtime `Library`.

## Invariants

- `TrackDetailSnapshot` contains one coherent selection kind, id set, aggregate field array, four owning credit sections, custom metadata set, common-tag ids, single-selection cover id, and `libraryRevision`.
  The revision follows the [captured-read revision semantics](../library/track-detail-projection.md#which-revision-does-a-snapshot-represent), not a guarantee of the latest library revision.
- Synthetic display fields and tags are excluded from the built-in field grid; tags have their own editing surface.
- Technical fields are never editable through metadata UI policy.
- Mixed built-in/custom values display the shared `<Multiple Values>` marker, and that literal cannot be committed as a custom value.
- `TrackPropertiesFormModel::setEditValue()` still omits mixed fields from `buildPatch()`.
  `setExplicitFieldEdit()` records a replacement that `buildPatch()` writes even when the field is mixed or equals the first target's value; a replacement equal to a common original is omitted.
  Both setters replace the same current edit value; a later `setEditValue()` clears explicit replacement intent and restores ordinary mixed-field preservation. `rowView()` continues to describe the captured baseline.
- Updating a custom key applies the value to every selected target; deletion removes it from every selected target.
- A custom key cannot be added when already present in the snapshot. Runtime metadata and Properties commands reject writes to `credits` or any exact TrackField id after NFC normalization, before mutation or interning, including previews with no targets. Matching is case-sensitive and does not accept query aliases. Deleting an existing reserved custom key remains available for cleanup; it never deletes the built-in field or structured Credits.
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
Each built-in raw field is an `AggregateValue<TrackFieldRawValue>`.
Credits use four `AggregateValue<vector<Credit>>` sections: section position supplies kind, and every contained entry agrees with it.
Complete sections compare name, kind, role, order, and duplicate multiplicity; present empty is a common value, while absence means no track was loaded.
A common whole-list value is assembled only when every section is common; no independently updated full-list mirror or per-track before-value cache is retained.
`LibrarySnapshot::trackCredits(id)` returns one owning canonical list and distinguishes a missing track from present empty credits.
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

### Preparing a coherent Properties baseline

A frontend must prepare the form from the same target sequence and committed revision that its authoring session bound:

1. Open one short-lived `LibrarySnapshot` from the target runtime's Core storage.
2. Pass that snapshot and the exact non-empty target sequence to the snapshot-aware `TrackAuthoringSession::begin()` overload.
   Runtime validates every id through that same snapshot, preserving order and duplicates, and stamps the binding with its revision and the current runtime identity.
   A snapshot from different Core storage is `InvalidInput`; a retained old snapshot or unavailable authoring state is `InvalidState` rather than a request to rebind at a newer revision.
3. Call `loadTrackPropertiesFormBaseline()` with that snapshot and the shared form specification.
   The loader validates every target before aggregating ordinary fields and complete per-kind Credits; success replaces the complete supplied form, while an empty or incomplete target set fails without changing it.
4. Read any editable shared tags from that same snapshot.
   Custom metadata uses its separate projection only when both the projection's exact target sequence and library revision match the binding.
5. Release the read snapshot before publishing native UI state, attach the session invalidation observer, and perform the final `isCurrent()` check before installing the baseline.

The session retains only the validated binding and its normal availability subscription, never the caller's read snapshot.
Coherent preparation does not prevent a commit after binding; the final current check and submission-time stale guard remain mandatory.
A caller using the target-only `begin()` overload must still require a subsequently opened baseline snapshot's revision to equal the bound revision; mismatches reject preparation, not silently rebind an existing draft or undo action.

The form specification, not the frontend, decides each row's editor kind.
Read-only rows remain visible baseline values but never enter the mutation patch, even if an adapter attempts to set an explicit edit.
A preparation failure installs no writable or surviving-subset baseline: GTK and WinUI keep their dialog long enough to present the error, while AppKit and TUI reject opening their editors.
Session ownership, subscriptions, native installation, and teardown remain frontend responsibilities.

### Built-in metadata

The frontend decodes edit text through the shared field codec and creates a typed `MetadataPatch`.
Applying the patch through the retained authoring session updates the complete bound target set or none of it.
The result is `Applied`, `NoOp`, `Busy`, `Stale`, or `Unavailable`; `Result` errors remain operational or validation failures.
An empty metadata display value remains hidden by default unless show-empty is active or its editor is open.

### Credits

`MetadataPatch::optCredits` is the only participant mutation: a typed `CreditReplacement` with a fixed four-kind mask and owning entries.
An absent patch preserves credits. A present mask must be nonempty, all kinds valid, and every entry inside its scope.
Each selected segment is replaced completely; a selected kind absent from entries is cleared, while every unselected segment is preserved independently per target.
All four bits mean full replacement. Empty entries clear only the selected scope, never RecordingDate or unrelated metadata.
Runtime normalizes once at the command boundary, compares complete affected segments for NoOp, and validates each resulting full record, not only the incoming list.
Bindings retain exact duplicate target occurrences, but mutation deduplicates first-seen targets before reconstruction or reuse of staged dictionary ids.
Failure on any target rolls back the entire command; one effective command commits and publishes once, while NoOp publishes nothing.

Category raw fields are read-only first-name text projections, not list values or editable scalars.
Compact previews use first name plus localized `+N`, counting duplicates; full detail/form surfaces use complete scoped sections instead.
A mixed category displays “Multiple values,” never the first target's compact summary, a union, or a common subset. Other common categories remain displayable and editable independently.

One shared `TrackCreditsEditorModel` handles empty, singleton, and larger lists:

| Entry | Immutable session scope | Kind control |
|---|---|---|
| Category preview | One kind | Hidden or locked |
| Full Credits | All four kinds | Editable |

Binding and owning baseline come from the same short-lived snapshot, released before UI work, submission, or waiting.
Opening or cancelling emits no replacement. A common scope loads every entry; a mixed scope starts unseeded and requires explicit **Replace this scope on all selected tracks** intent.
An untouched empty mixed replacement draft cannot clear anything; a distinct **Clear this scope** action shows scope and targets.
Explicit Clear intent lasts until that session ends: adding and then deleting the final row still permits the clear, while the same roundtrip after Replace alone does not.
Name edits preserve kind and role. Add/delete/move preserve unedited attributes; moves are within a kind, and reclassification in the all-kind editor appends to the destination segment.
Invalid rows prevent Save while preserving draft, focus, and error information.
Validation uses typed reasons for blank names, invalid name or role text, and invalid kinds; localized messages identify the affected draft row with one-based numbering rather than exposing a raw normalization diagnostic.
No inline scalar category edit or parsing of a preview string is permitted.

Busy preserves the draft and binding. Any intervening revision makes it stale, even when an unrelated category changed; there is no optimistic merge or silent rebinding.
Reload must be explicitly confirmed before discarding a draft, and failed preparation must not install a surviving subset or a fresh binding over the old draft.
WinUI and TUI offer confirmed reload. AppKit instead requires confirmed close/discard and reopening Properties, with no inline Reload.
GTK stale Credits require cancelling and reopening the editor; snapshot refresh never reseeds the live draft.

Scoped clear Undo has the existing narrow contract: capture one common nonempty scoped pre-value coherently with binding and restore typed entries with the same mask through the post-commit session.
A mixed scope has no offer because no per-track before-values are retained; unrelated mixed kinds do not disqualify a common edited scope.
No diagnostic report string or live transaction becomes Undo data. Failure never offers Undo, and later revisions/selection changes retire or stale it under the existing rules.
GTK detail provides that offer and its shared timer/settlement behavior; WinUI, TUI, and AppKit do not expose a new Credits Undo UI.

### Custom metadata

Addition first validates duplicate and reserved-key conflicts.
Update creates `customUpdates[key] = value`; delete creates `customUpdates[key] = nullopt`.
An empty `changes` list is a no-op; each applied entry identifies its track and changed fields or tags.

UIModel builds that delete patch and does not compute or return a custom-delete undo value.
GTK computes the prior value by walking each bound target's stored custom value in the same short-lived library snapshot used to bind the session.
A key present on every bound target with one common stored value, including a present empty value, is eligible; mixed or missing values may still be deleted, without an undo offer.
The offer is published only after `Applied`, and only while that selection still matches.
An applied deletion transfers its session, now holding the next-revision binding, into the pending undo state.
Replay submits the reverse patch through that same guarded session.
The [GTK track-detail specification](../frontend/gtk/track-detail.md) owns the mutually exclusive custom-delete and Credits-clear opportunity, the shared bar, the five-second timer, and the revision, `Busy`, stale, selection, and replay guards.
Any intervening effective commit makes an offered session stale instead of overwriting newer work.

### Tags

`applyTagEditAsync()` submits additions/removals through the targets already bound to its `TrackAuthoringSession`; it does not copy or rebind a second selected-id set.
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

A Properties coordinator owns ordinary fields, tags, and Credits under one captured authoring session.
It permits one active Credits child; accepting that child stages the parent draft rather than writing the database.
Sequential accepted scopes overlay one pending replacement: union masks, replace newly edited segments, and retain previously staged other segments. Full-scope acceptance replaces all staged segments.
Reopening reads pending values where staged and the captured baseline elsewhere; child Cancel preserves prior accepted scopes and ordinary dirty fields without rereading storage.
An active child blocks parent Save and programmatic patch submission, including when only ordinary metadata or tags are dirty.
After the child is accepted, Save submits one composed `TrackPropertiesPatch` through the retained session.
Runtime applies metadata, Credits, and tags in one write transaction and publishes at most one revision and changeset.
The form closes only after that command returns `Applied` or `NoOp`; `Busy`, stale/unavailable state, and `Result` errors leave the form open or disabled according to the session state without representing either part as committed.

## Failure and cancellation

Runtime mutation failure rejects the edit and exposes the recoverable diagnostic to the frontend workflow.
For a combined Properties save, failure after either part has staged changes aborts the complete write transaction, so reusing the unchanged form draft cannot resubmit an already committed metadata half.
No partial frontend state is treated as committed merely because an editor closed.
GTK table inline edits place parsing, operational, stale, and unavailable failures in the table's existing status surface and update the row only after `Applied`.
GTK detail-grid parsing and submission failures create an error notification and restore the pre-edit display value; the backing library remains unchanged on rejection.
Detail undo returns a stale, unavailable, or operational terminal failure, clears that same opportunity, and the undo bar publishes that failure as an error notification. A completion bound to a displaced opportunity does not clear its replacement. A `Busy` result reports temporary resource contention without clearing the pending undo.
The current synchronous mutation boundary has no cancellation token; cancellation before submission discards the local draft, while a returned successful mutation is committed.
Destroying or moving the facade after submission does not itself cancel the command, because the coroutine retains Session State; the owning frontend workflow must settle that task before destroying the runtime `Library` borrowed by the State.

Stale and unavailable outcomes tell the frontend to reload rather than retry the same session.
Missing targets are rejected with `NotFound` while creating the binding.
Once an exact-revision binding exists, target disappearance without a newer committed revision is an invariant violation rather than a frontend-recoverable status.
After durable commit, a publication failure faults the runtime; UIModel cannot treat it as an ordinary uncommitted rejection.
The authoring session invalidates itself and propagates that exception to its caller; it does not translate the failure into an `Applied` result.

## Persistence and versioning

Built-in metadata, custom metadata, and tags persist in the library through the library mutation contract.
They are also represented by governed YAML transfer according to the [library YAML transfer specification](../library/yaml-transfer.md).
Editor visibility, expansion, drafts, and delete-undo state are not library data.

## Frontend observations

A frontend may distinguish no selection, mixed values, partial custom-key presence, empty metadata, and technical unknowns.
It may choose inline, form, or command interaction for ordinary scalar fields while using the same schema, codec, validation, and runtime writer authority.
Credits always use the shared scoped list editor. GTK, TUI, WinUI, and AppKit have native controls for name/role editing, add/delete, within-kind reorder, and all-kind reclassification; their detail routes reach that editor directly or through Properties.

WinUI presents one native `ContentDialog` for a captured single- or multi-track selection.
The dialog exposes editable built-in metadata, tags common to every target, custom metadata, and read-only technical properties.
It opens from the row context menu, an ordinary shell menu, or the window-local `Alt+Enter` accelerator; right-clicking an unselected row selects that row before opening its menu.
Save is disabled while the draft is unchanged or invalid, remains open across `Busy` and recoverable failures, and closes only after the combined Properties submission is accepted.
A stale session disables submission and preserves the draft. Its Reload action requires a discard decision before rebuilding the baseline and binding for the captured targets; declining keeps the draft.
WinUI detail category/all-kind actions open Properties with the requested child scope.
AppKit inspector **Edit Properties** opens its [native Properties workflow](../frontend/appkit-track-properties.md), including from the compact inspector sheet; it has no direct scoped Credits action in the inspector.
Inspector display uses the [detail projection's aggregation and missing-track rules](../library/track-detail-projection.md#aggregation), while opening an editor still requires every captured target.
Stale recovery is confirmed close and reopen, not an inline reload.

Custom keys are queryable through the custom-variable syntax in the predicate language; presentation does not reinterpret or restrict that grammar.
Locale ordering affects only suggestion and chooser position. It never changes
tag equality, matching, stored tag bytes, or mutation semantics.

## Implementation map

- [`TrackDetailProjection.h`](../../../app/include/ao/rt/projection/TrackDetailProjection.h) defines the aggregate snapshot.
- [`TrackDetailProjection.cpp`](../../../app/runtime/projection/TrackDetailProjection.cpp) builds and observes live snapshots.
- [`TrackFieldGrid.cpp`](../../../app/uimodel/library/detail/TrackFieldGrid.cpp) and [`TrackFieldGrid.h`](../../../app/include/ao/uimodel/library/detail/TrackFieldGrid.h) own field selection and visibility.
- [`TrackAuthoring.h`](../../../app/include/ao/uimodel/library/track/TrackAuthoring.h) owns edit decoding, writable-field classification, patch construction, and inline mixed-value protection.
- [`TrackPropertiesFormModel.h`](../../../app/include/ao/uimodel/library/property/TrackPropertiesFormModel.h) and [`TrackPropertiesFormSpec.h`](../../../app/include/ao/uimodel/library/property/TrackPropertiesFormSpec.h) own compact form state, all-or-none standard-field baseline loading, mixed-value policy, explicit replacement intent, editor kinds, and patch construction.
- [`TrackCustomMetadata.cpp`](../../../app/uimodel/library/detail/TrackCustomMetadata.cpp) owns display, validation, and update/delete patches. It has no undo API.
- [`TrackCredits.h`](../../../app/include/ao/uimodel/library/detail/TrackCredits.h) owns shared scoped list editing, typed row validation and localized messages, complete per-kind baselines, detail/summary formatting, and common-scope clear-undo eligibility. It does not own the pending offer.
- [`TagEdit.cpp`](../../../app/uimodel/library/property/TagEdit.cpp) owns tag mutation submission and status text.
- [`TrackAuthoringSessions.h`](../../../app/include/ao/uimodel/library/track/TrackAuthoringSessions.h) owns the move-only value facade, stable targets, and bound revision; [`TrackAuthoringSession.cpp`](../../../app/uimodel/library/track/TrackAuthoringSession.cpp) owns shared asynchronous State, current-binding lifetime, invalidation, and result mapping.
- [`LibraryCommandsTrackAuthoring.cpp`](../../../app/runtime/library/LibraryCommandsTrackAuthoring.cpp) owns metadata, tag, and combined-properties mutation commit.
- [`TrackEditController.cpp`](../../../app/tui/TrackEditController.cpp) owns the TUI's coherent snapshot preparation, retained session, and submission, under the [TUI track-authoring specification](../frontend/tui-track-authoring.md).
- [`TrackPropertiesCoordinator`](../../../app/windows-winui/track/TrackPropertiesCoordinator.h) owns the native dialog and guarded asynchronous workflow; [`TrackPropertiesAdapter`](../../../app/windows-winui/include/ao/winui/track/TrackPropertiesAdapter.h) maps shared form and vocabulary state without WinRT.

## Test map

- Runtime projection tests under [`test/unit/runtime/projection/`](../../../test/unit/runtime/projection) protect aggregation and refresh.
- [`TrackFieldGridSchemaTest.cpp`](../../../test/unit/uimodel/library/detail/TrackFieldGridSchemaTest.cpp) and [`TrackFieldGridVisibilityTest.cpp`](../../../test/unit/uimodel/library/detail/TrackFieldGridVisibilityTest.cpp) protect field/visibility policy.
- [`TrackAuthoringTest.cpp`](../../../test/unit/uimodel/library/track/TrackAuthoringTest.cpp) protects edit decoding, writable-field coverage, patch construction, and mixed-value sentinels.
- [`TrackPropertiesFormModelTest.cpp`](../../../test/unit/uimodel/library/property/TrackPropertiesFormModelTest.cpp) protects mixed-value omission, explicit mixed-field replacement, and common no-op omission.
- [`TrackPropertiesFormBaselineTest.cpp`](../../../test/unit/uimodel/library/property/TrackPropertiesFormBaselineTest.cpp) protects all-target validation, unchanged output on failure, ordered duplicate targets, empty and numeric-zero values, mixed aggregation, and read-only exclusion from patches.
- [`TrackCustomMetadataTest.cpp`](../../../test/unit/uimodel/library/detail/TrackCustomMetadataTest.cpp) protects validation, patches, and mixed values. It has no undo case.
- [`TrackCreditsTest.cpp`](../../../test/unit/uimodel/library/detail/TrackCreditsTest.cpp) protects scoped editing, typed validation and localized one-based row messages, complete equality, mixed replacement intent, attribute preservation, and common-scope clear-undo eligibility.
- [`TrackPropertiesCreditsTest.cpp`](../../../test/unit/uimodel/library/property/TrackPropertiesCreditsTest.cpp) protects sequential scope overlays, pending-baseline reopening, child cancellation, and active-child Save exclusion.
- [`TrackPerformanceAuthoringTest.cpp`](../../../test/unit/runtime/library/TrackPerformanceAuthoringTest.cpp) protects typed scoped mutation, retained segments, canonical NoOp, bounds, and atomicity.
- GTK undo coverage is mapped by the [GTK track-detail specification](../frontend/gtk/track-detail.md#test-map).
- [`TagEditTest.cpp`](../../../test/unit/uimodel/library/property/TagEditTest.cpp) protects tag mutations and statuses.
- [`TrackAuthoringSessionTest.cpp`](../../../test/unit/uimodel/library/track/TrackAuthoringSessionTest.cpp) protects stable target order, bound revision, no-op reuse, successful binding advancement, invalidation after another commit, move-only facade semantics, and a pending submission settling after moved and destroyed facades.
- [`TrackAuthoringSnapshotTest.cpp`](../../../test/unit/runtime/library/TrackAuthoringSnapshotTest.cpp) protects snapshot-owned target validation, ordered duplicates, foreign-storage rejection, retained-old-snapshot rejection, runtime identity, lifecycle, and maintenance gates.
- [`TrackAuthoringSnapshotSessionTest.cpp`](../../../test/unit/uimodel/library/track/TrackAuthoringSnapshotSessionTest.cpp) protects same-snapshot owning form preparation, snapshot release, no-op/applied reuse, and stale-draft rejection without rebinding.
- [`LibraryCommandsTest.cpp`](../../../test/unit/runtime/library/LibraryCommandsTest.cpp) protects committed multi-target behavior.
- [`LibraryCommandsTrackPropertiesTest.cpp`](../../../test/unit/runtime/library/LibraryCommandsTrackPropertiesTest.cpp) protects combined metadata/tag publication and rollback when the later tag stage fails.
- [`TrackPropertiesAdapterTest.cpp`](../../../test/unit/winui/track/TrackPropertiesAdapterTest.cpp) protects WinUI control projection, mixed values, edit parsing, command availability, commit-state mapping, and tag/custom-key completion without WinRT.

## Related documents

- [Presentation architecture](README.md)
- [Library architecture](../library/structure.md)
- [Track model reference](../../reference/library/model/track.md)
- [Track field reference](../../reference/library/model/track-field.md)
- [Predicate language reference](../../reference/query/predicate-language.md)
- [GTK track-detail specification](../frontend/gtk/track-detail.md)
- [TUI track authoring](../frontend/tui-track-authoring.md)
- [Windows Properties](../frontend/windows.md)
- [AppKit Properties and Credits](../frontend/appkit-track-properties.md)
