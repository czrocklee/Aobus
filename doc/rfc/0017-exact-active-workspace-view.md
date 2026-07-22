---
id: rfc.0017.exact-active-workspace-view
type: rfc
status: draft
domain: workspace
summary: Proposes restoring the exact active entry from the ordered workspace view list.
depends-on: none
---
# RFC 0017: Exact active workspace view

## Problem

Workspace state stores ordered `openViews`, but identifies the active view only by `activeListId`.
Two open views can use the same list with different filters or presentations.
On restore, `WorkspaceService` scans for a matching list and gives a filtered match priority, so it can focus a different entry from the one that was active when saved.
The wrong entry also seeds the initial navigation history.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Restore the exact active entry from `openViews`.
- Keep runtime-generated `ViewId` values out of persisted state.
- Reject an invalid active entry before creating candidate views.
- Preserve current ordered reconstruction, single workspace commit, and initial-history behavior.

## Non-goals

- Add root versioning, library binding, or collection limits.
- Persist navigation history, selection, playback, or widget state.
- Give every stored view a durable identity.
- Change GTK and TUI session lifecycle.

## Proposed design

Replace root field `activeListId` with unsigned `activeViewIndex`.
The value is a zero-based index into `openViews`; an empty list uses the canonical value `0`.

Serialization finds the active `ViewId` in the ordered live snapshot and writes its position.
A nonempty workspace whose active id is absent from `openViews` is invalid and is not saved.

Deserialization requires `activeViewIndex < openViews.size()` when the sequence is nonempty and requires `0` when it is empty.
This validation happens before `ViewService` creates any candidate.

Restore creates views in serialized order and selects `createdViewIds[activeViewIndex]` directly.
The selected view's exact filter and presentation then seed history through the existing commit path.
The list-matching heuristic is removed.

## Alternatives

### Persist `ViewId`

`ViewId` belongs to one runtime instance and cannot identify a reconstructed view.

### Add a durable key to every view

That solves a broader identity problem that current session behavior does not have.
The ordered sequence already supplies the required local identity.

### Keep `activeListId` and refine the heuristic

No list-based rule can distinguish two entries with the same list and different view state.

## Compatibility and migration

The strict workspace mapping changes in place.
Documents containing `activeListId` are rejected rather than guessed or migrated; the next successful checkpoint writes `activeViewIndex`.
The nested `presentationVersion` and presentation vocabulary do not change.

## Validation

- Round-trip two views over the same list with different filters and presentations, with each entry active in turn.
- Prove the restored active entry and first history point are exact.
- Reject an out-of-range index before candidate creation and publication.
- Round-trip an empty workspace with index `0`.
- Keep candidate cleanup and single-restore-publication tests passing.

## Promotion plan

Update the workspace architecture, session specification, and session-state reference with the exact field and restore rule, then delete this RFC after implementation.
