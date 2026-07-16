---
id: linux-gtk.track-detail
type: spec
status: current
domain: presentation
summary: Defines GTK track-detail field-grid, inline editor, custom-metadata undo, tag-chip flow, constrained sizing, and shell composition.
---
# GTK track-detail specification

## Scope

This specification owns the GTK adaptation of the [metadata-editing contract](../presentation/metadata-editing.md): detail-scope composition, field-grid layout, inline editing, custom-metadata controls and undo, tag-chip flow, constrained sizing, and cover-art footprint.
It does not own aggregation, patch semantics, query syntax, or library mutation.

## Code boundary

Declarative shell types place `track.detailScope`, `track.fieldGrid`, `track.detailUndoBar`, `track.tagEditor`, and cover-art components.
GTK implementations under `app/linux-gtk/layout/component/track/`, `app/linux-gtk/tag/`, and `app/linux-gtk/track/` adapt one runtime `TrackDetailProjection` and UIModel policies.

The detail scope owns a projection subscription and one undo controller for its descendant components.
The field grid and tag editor borrow that scope and create UIModel authoring sessions from its exact selected ids when an edit begins.
They do not construct storage, call `LibraryWriter`, or create a competing detail snapshot.

## Terminology

- The **detail scope** is the shell subtree sharing one selected-track snapshot and delete-undo owner.
- The **field grid** is the built-in/custom metadata and technical-property surface.
- A **detail editor** is a display label plus transient inline entry and edit button.
- The **tag flow** is the height-for-width custom chip layout.
- The **action row** contains show/hide-empty and add-custom-metadata controls.

## Invariants

- No selection keeps the same cover/grid/tag structure in place but disables mutation.
- The field grid uses one field per row at every width and does not create its own scroll boundary.
- Metadata is expanded by default; technical properties are collapsed by default.
- Built-in metadata, the action row, and custom metadata belong to one Metadata section.
- Only an explicit edit button begins built-in/custom inline editing; value text is not a hidden activation target.
- One detail editor is active at a time; opening another commits the previous editor first.
- Enter and outside click commit; Escape cancels; literal `<Multiple Values>` is not saved.
- A session binds targets when editing begins; selection/revision change, maintenance, or runtime fault makes it stale and prevents commit.
- Custom deletion undo is offered only when the prior value is unambiguous across the complete selection.
- Detail selection change, timeout, successful undo, stale undo, or overlapping rewrite clears pending undo.
- The field grid, tag flow, and cover slot report compressible horizontal minima and cannot widen the collapsible detail pane from content.
- Invalid UTF-8 is replaced for GTK/Pango display without rewriting stored bytes until the user commits an edit.

## State model

The field grid retains expanded/collapsed section state, show-empty state, generated row/editor objects, current snapshot, custom-add popover state, and one active-editor coordinator.
The undo controller retains one key, unambiguous prior value, the applied edit's next-revision `TrackAuthoringSession`, and a five-second timer.

The tag editor retains current and suggested chip models, open add/search state, filter text, top-level outside-click watch, and theme-derived inter-chip gap.
Its child order is current tags, suggested tags, then the persistent add trigger/entry.

## Commands and transitions

### Field display and editing

Built-in field rows come from UIModel schema; synthetic display fields and tags are excluded.
Mixed values display `<Multiple Values>`.
Missing technical values display `Unknown`; missing metadata displays empty and is hidden until show-empty is active.
An editor remains visible while active even if its prior display value was empty.

Editable rows reveal edit controls on hover or keyboard focus.
Custom rows additionally reveal a delete action and show a warning icon when the key is missing from part of the selection.
The add action opens a key/value popover and rejects duplicate or reserved keys through UIModel validation.

Opening a built-in or custom editor captures the current detail snapshot and starts a session for that snapshot's exact target order.
Commit submits only through that retained session.
If the authoritative library revision changes while the editor is open, GTK restores authoritative display state and requires a new edit rather than rebinding the existing text.

Deleting an undo-eligible custom key publishes pending undo in sibling `track.detailUndoBar` for five seconds.
Undo restores the key/value through the session returned by the delete commit.
An intervening effective commit makes that session stale, so undo cannot overwrite newer library state.

### Sections and layout

Content order is Metadata header, built-in metadata rows, action row, selected-track custom rows, Audio Properties header, then technical rows.
Collapsed headers remain visible and show a selection-derived summary; expanding/collapsing never changes field order.

All rows use stable grid coordinates and clipped fixed-height hosts.
Key/value natural-width anchors include hidden/collapsed content but contribute zero minimum width, preventing column jumps while allowing extreme compression.
Values ellipsize and expose full display text in tooltips.

The default layout owns scrolling around `track.fieldGrid` and leaves `track.detailUndoBar` outside it so undo remains visible.
Sibling cover and tag components remain outside the field-list scroll according to the authored layout.

### Tag flow

Current chips are inert except for their dedicated remove button.
Suggested chips use an add affordance and promote into current state when the mutation succeeds.
The trailing `Add…` trigger swaps to an entry; Enter submits and keeps the cleared entry focused for rapid additions, while Escape, focus traversal, or outside press dismisses without submission.

While the entry is open, current chips are hidden and suggestions are filtered by case-insensitive substring.
Each visible child receives its own natural width clamped to the line, so a wide entry does not stretch adjacent chips.
Classic uses a dense inter-chip gap and Modern a wider gap; intra-chip spacing is independent.

The tag controller starts one authoring session for the exact selection it displays.
Every add/remove event must still match those session targets; selection change or stale availability closes the edit path rather than applying to a new selection.

## Failure and cancellation

Rejected, missing, stale, and unavailable runtime edits leave the authoritative snapshot unchanged and surface their workflow state through the frontend's reporting path.
Closing/canceling an entry before submission discards its draft.
Outside-click watches exist only while an editor/add entry is active and are removed on close/destruction.
Undo failure clears no committed library mutation and remains subject to runtime writer reporting.

## Persistence and versioning

Metadata and tags persist only through runtime library mutation.
Field-section expansion, show-empty state, editor drafts, tag search, and pending undo are detail-scope UI state and are not serialized.
Shell structure and component ids/properties belong to the layout document reference.

## Frontend observations

The field grid uses a four-column GTK grid: labels in the first column and values/actions in the remaining columns according to row kind.
Technical rows have no edit affordance and are visually dimmer.
Section headers keep a full-width rule, summary label, and disclosure chevron without allowing the chevron to shift the rule.

The detail cover slot is a deterministic responsive square capped by its target size; source aspect ratio, missing art, and selection changes do not change its footprint.

## Implementation map

- [`TrackDetailScope.cpp`](../../../app/linux-gtk/layout/component/track/TrackDetailScope.cpp) owns snapshot and undo scope.
- [`TrackFieldGridComponent.cpp`](../../../app/linux-gtk/layout/component/track/TrackFieldGridComponent.cpp) owns field-grid composition and commands.
- [`TrackDetailUndo.cpp`](../../../app/linux-gtk/layout/component/track/TrackDetailUndo.cpp) owns pending undo and timeout.
- [`TrackAuthoringSession.h`](../../../app/include/ao/uimodel/library/property/TrackAuthoringSession.h) owns the target/revision binding used by GTK editors and undo.
- [`TagEditor.cpp`](../../../app/linux-gtk/tag/TagEditor.cpp) owns chip flow and add/search interaction.
- [`TrackTagEditorComponent.cpp`](../../../app/linux-gtk/layout/component/track/TrackTagEditorComponent.cpp) binds tag editor to detail scope.
- [`TrackCoverArtComponent.cpp`](../../../app/linux-gtk/layout/component/track/TrackCoverArtComponent.cpp) owns cover-slot adaptation.

## Test map

- [`TrackFieldGridCollapsibleTest.cpp`](../../../test/unit/linux-gtk/layout/components/TrackFieldGridCollapsibleTest.cpp) protects sections and empty-field visibility.
- [`TrackDetailConstrainedLayoutTest.cpp`](../../../test/unit/linux-gtk/layout/components/TrackDetailConstrainedLayoutTest.cpp) protects stable rows, clipping, anchors, and narrow allocation.
- [`TrackFieldGridTextTest.cpp`](../../../test/unit/linux-gtk/layout/components/TrackFieldGridTextTest.cpp) protects mixed/unknown/display text.
- [`SemanticLayoutComponentsTest.cpp`](../../../test/unit/linux-gtk/layout/components/SemanticLayoutComponentsTest.cpp) protects scope, mutations, undo, and shell binding.
- [`TagEditorTest.cpp`](../../../test/unit/linux-gtk/tag/TagEditorTest.cpp) and [`TagEditControllerTest.cpp`](../../../test/unit/linux-gtk/tag/TagEditControllerTest.cpp) protect chip flow and edit interaction.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Application shell architecture](../../architecture/application-shell.md)
- [Metadata-editing specification](../presentation/metadata-editing.md)
- [Shell layout adaptation](../shell/layout-adaptation.md)
- [Cover-art resource delivery](../resource/cover-art-delivery.md)
- [Layout catalog reference](../../reference/shell/layout-catalog.md)
