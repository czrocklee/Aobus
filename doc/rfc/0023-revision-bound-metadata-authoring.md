---
id: rfc.0023.revision-bound-metadata-authoring
type: rfc
status: draft
domain: presentation
summary: Proposes revision-bound metadata and tag commands with explicit target, conflict, missing, and undo outcomes across presentation and library boundaries.
depends-on: rfc.0022.transaction-coherent-library-dictionary
---
# RFC 0023: Revision-bound metadata authoring

## Problem

Aobus has shared field schemas, aggregate detail snapshots, typed metadata patches, and one runtime writer, but the authoring command is not bound to the state from which the edit was derived.

`TrackDetailSnapshot` contains selected track ids and a projection revision.
GTK and UIModel editors capture values from that snapshot, then call `LibraryWriter::updateMetadata(trackIds, patch)` or `editTags(trackIds, ...)` without the observed revision, library generation, or expected field values.
The writer opens a fresh transaction and applies the patch to whatever records currently occupy those ids.

This creates several indistinguishable outcomes:

- a selection or detail snapshot changed while an inline editor remained open;
- another authoring surface changed the same field after the draft was created;
- a target was deleted, reimported, or replaced before submission;
- an active-library replacement invalidated the lifetime behind a delayed frontend action; and
- an undo action replays an old value over a newer edit.

The current writer silently skips missing track ids.
Its reply lists only `mutatedIds` and field changes, so a multi-target edit cannot distinguish "all targets already had this value" from "some targets disappeared" without re-reading and reconstructing intent.
UIModel workflows generally reduce the result to whether `mutatedIds` is empty.

Undo is especially weak.
Custom-metadata deletion records ids, key, and a representative previous value when the initial aggregate is uniform, then restores that value through the same unconditional writer call.
It does not prove that the deletion is still the latest write or that the ids still denote the same authoring targets.

The command edits the library database only; `media::file::File` remains a read-only ingestion boundary and source audio files are not rewritten.
That ownership is sound but easy to obscure when the end-to-end authoring contract is split across presentation and library documents.

## Dependencies

- Hard: [RFC 0022](0022-transaction-coherent-library-dictionary.md).
- Conditional: None.
- Integration: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0013](0013-coherent-application-reporting-policy.md), [RFC 0016](0016-coherent-workspace-transactions.md).

RFC 0022 must make dictionary changes rollback with a rejected or conflicted authoring transaction; otherwise a revision guard can reject the track mutation while still leaking metadata strings into process state.
RFC 0003 should carry the authoring receipt and committed revision through the unified mutation pipeline.
RFC 0013 should map conflict, missing, rejected, and storage outcomes to one cross-frontend reporting policy.
RFC 0016 should align selection/view revisions when a command originates from workspace state.

## Goals

- Bind a metadata/tag command to the library instance and observed detail state from which the user formed it.
- Detect concurrent field changes, missing/replaced targets, and stale library generations before mutation.
- Define one atomic multi-target authoring policy.
- Return typed per-command evidence for applied, no-op, conflict, missing, rejected, and failed outcomes.
- Make undo a compare-and-restore command rather than an unconditional replay.
- Keep frontend drafts and validation in UIModel while retaining mutation authority in runtime/library.
- Preserve the current rule that authoring changes the Aobus library database, not source audio files.
- Support GTK, TUI, and future automation surfaces without embedding widget concepts in runtime.

## Non-goals

- Write ID3, Vorbis Comment, MP4, or other tags back to source media files.
- Add collaborative multi-user editing or distributed conflict resolution.
- Persist editor drafts, popover state, or undo history as library truth.
- Make projection revision equal to library revision when they have different owners.
- Replace field validation, formatting, or aggregate presentation policy already owned by UIModel.
- Redesign scan/import conflict handling, except where reimport changes an authoring target before commit.

## Proposed design

### Authoring command envelope

Introduce a runtime `MetadataAuthoringCommand` (with a parallel tag shape or one typed variant) containing:

```text
command id
library identity + runtime generation
target TrackIds
observed detail/projection revision
optional observed library revision
typed patch or tag delta
expected values for every field/key the command may overwrite
intent kind: edit, delete, or undo
```

The frontend/UIModel builds the command from one immutable `TrackDetailSnapshot` and its local validated draft.
It cannot substitute the currently focused selection at submit time.
Stable ids remain explicit command data.

The runtime generation protects the object/lifetime boundary.
The library revision detects broad staleness when available.
Per-field expected values provide the semantic compare condition, avoiding false conflicts when an unrelated track or field changed after the snapshot.

### Expected-value model

For each target and changed field, the command records the state the editor observed:

- exact typed value for a present built-in field;
- absent or exact string value for a custom key;
- present/absent membership for each changed tag; and
- an explicit mixed/unknown precondition only when the authoring policy can define it safely.

For multi-selection aggregate editing, UIModel obtains a compact baseline from the detail projection or requests a runtime authoring token that represents the per-target expected values without exposing a second frontend read loop.
The token is library-generation-bound and cannot be reused for a different patch surface.

The baseline is evidence, not a lock.
Runtime validates it inside the same write transaction that would apply the patch.

### Atomic validation and commit

Use all-or-none semantics for one authoring command:

1. verify command and runtime/library generation;
2. open one library write transaction;
3. load every explicit target;
4. reject if a target is missing or its guarded value differs;
5. validate and prepare every field/tag/dictionary mutation;
6. determine whether the command is a semantic no-op;
7. commit all changed targets and dictionary rows together; and
8. publish one library revision and authoring receipt.

The writer no longer silently skips a missing target for authoring commands.
Bulk administrative APIs may retain a separately named best-effort policy, but an interactive authoring surface cannot accidentally call it.

Unrelated field changes do not conflict unless the command declares a whole-record guard.
This permits two editors to change disjoint fields on the same track while preventing lost updates to the same field.

### Typed outcome and receipt

Return one of:

```text
Applied {
  commandId, libraryRevision, targetIds, fieldChanges
}
NoOp {
  commandId, targetIds
}
Conflict {
  commandId, conflicting target/field evidence, current revision
}
Missing {
  commandId, missing targetIds
}
Rejected {
  commandId, validation reason
}
Failed {
  commandId, operational error
}
```

Conflict evidence identifies stable track and field/key/tag identities but need not return every current value when that would expose a large or stale replacement snapshot.
The frontend refreshes from the authoritative projection and may offer retry, overwrite, or cancel according to product policy.

Only `Applied` creates an undo receipt.
`NoOp` is successful but creates no history item.

### Compare-and-restore undo

An applied receipt records both the prior value and the value written for each changed target/field.
Undo constructs a new authoring command whose expected values are the values written by the original command and whose patch restores the prior values.

If another edit has changed any guarded value, undo returns `Conflict` and does not overwrite that edit.
Undo uses a new command id and committed library revision; it is not transaction rollback.

For multi-target edits, undo retains the original atomic policy.
It either restores every target still matching the applied receipt or changes none.

### Projection and selection interaction

`TrackDetailSnapshot::revision` remains projection-owned.
The detail projection additionally exposes the library/runtime identity and authoring baseline or token needed by the command.
Changing selection invalidates an editor bound to the previous snapshot unless the editor deliberately retains an explicit-target authoring session.

Inline row edits bind to the row identity and baseline captured at edit start, not the row object currently occupying a recycled list position.
Modal track-property dialogs retain their explicit id set for their whole lifetime and surface a stale/conflict result rather than retargeting.

Active-library replacement closes authoring admission for the old runtime generation before destroying its projections and writers.
A late callback receives a stale-generation outcome and performs no write or notification against the replacement library.

### Ownership and source-file boundary

UIModel continues to own field schema, text decoding, mixed-value policy, draft construction, and user-action recommendations.
Runtime owns generation checks, baseline validation, transaction orchestration, receipts, and library change publication.
Core library owns record and dictionary integrity.
GTK/TUI own editor lifetime and conflict presentation.

No command in this proposal writes through `media::file::File`.
If source-file authoring is ever added, it requires a separate transactional/file-consistency design and an explicit user-facing operation.

## Alternatives

### Guard only on library revision

A whole-library revision prevents every lost update but creates conflicts for unrelated scans, list edits, or changes to other tracks.
It is useful coarse evidence but not sufficient authoring semantics.

### Last writer wins

This preserves the current simple API but silently loses concurrent edits and makes undo destructive.
Refreshing after commit cannot recover the overwritten value.

### Best-effort per-target updates

Applying to surviving targets can be desirable for administrative cleanup, but interactive multi-edit would no longer mean what the confirmation surface displayed.
A separately named bulk policy is clearer than an implicit partial outcome.

### Lock records while an editor is open

Long-lived locks across UI interaction block scans and other editors, complicate teardown, and do not survive process boundaries.
Optimistic compare-and-commit fits the existing transaction model.

### Re-read in each frontend immediately before submit

The value can change again between the re-read and write, and every frontend would duplicate conflict policy.
Validation must occur inside the owning write transaction.

## Compatibility and migration

Existing metadata and tag records require no storage migration.
The runtime writer API changes, and UIModel/GTK call sites migrate from ids-plus-patch to an authoring command or prepared session.

CLI bulk update can initially retain its explicit current command semantics through a deliberately named unguarded administrative mode.
If CLI later exposes guarded automation, RFC 0029 defines how baselines and conflicts appear in the versioned protocol.

Editor behavior changes on races: an operation that previously applied to the current record or silently skipped a missing id now returns a visible typed outcome.
Normal single-editor success and no-op behavior remains unchanged.

Undo receipts are ephemeral unless a later product requirement gives them a managed-state owner.
Existing frontend-local undo state can be discarded on upgrade.

## Validation

- An edit derived from a current detail snapshot commits and returns the matching new library revision.
- A concurrent change to the same field produces `Conflict` and commits no target.
- A concurrent change to an unrelated field can commit when the command uses field-level guards.
- Missing, deleted, or reimport-replaced targets produce `Missing` or `Conflict` with no partial mutation.
- Multi-selection tests prove all-or-none behavior when one target is stale, missing, invalid, or already changed.
- Changing the focused selection while an editor is open cannot retarget the command.
- An old-runtime command after active-library replacement is rejected before touching the new library.
- Undo succeeds only when every written value still matches and conflicts after a later edit.
- Preview/conflict/failure paths leave dictionary state unchanged under RFC 0022.
- GTK inline, detail-grid, custom-metadata, tag, and property-dialog tests cover stale and refresh presentation.
- Runtime mutation/change tests prove one commit, one revision, and one publication per applied command.
- Source audio fixture bytes remain unchanged after every metadata/tag authoring test.
- A full `./ao check` passes after migration.

## Open questions

- Should the public command carry explicit per-target baselines or an opaque runtime-issued authoring token?
- Which operations need whole-record guards instead of field-level expected values?
- Should a conflict response expose current typed values, only identities/revisions, or a bounded refresh token?
- Does CLI bulk metadata update remain explicitly unguarded, or should all mutation clients supply an observed baseline?
- How long should frontend undo receipts live, and should one new edit invalidate only overlapping receipts or the whole local stack?

## Promotion plan

If accepted and implemented:

- update the [library architecture](../architecture/library.md) and [presentation architecture](../architecture/presentation.md) with the end-to-end authoring command, generation, and receipt boundaries;
- update the [metadata-editing specification](../spec/presentation/metadata-editing.md) with baselines, conflict behavior, atomic multi-target semantics, and compare-and-restore undo;
- update the GTK track-detail and dialog specifications with stale editor and conflict presentation;
- update library mutation specifications with guarded validation and one committed revision/change publication;
- update CLI specification/reference if guarded authoring becomes part of its automation surface; and
- record the selected baseline representation and atomicity policy in a decision if accepted.
