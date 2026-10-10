---
id: linux-gtk.track-detail
---
# GTK track-detail specification

## Scope

This specification owns the GTK adaptation of the [metadata-editing contract](../../presentation/metadata-editing.md): detail-scope composition, field-grid layout, inline editing, custom-metadata controls and undo, tag-chip flow, constrained sizing, and cover-art footprint.
It does not own aggregation, patch semantics, query syntax, or library mutation.

## Code boundary

Declarative shell types place `track.detailScope`, `track.fieldGrid`, `track.detailUndoBar`, `track.tagEditor`, and cover-art components.
GTK implementations under `app/linux-gtk/layout/component/track/`, `app/linux-gtk/tag/`, and `app/linux-gtk/track/` adapt one runtime `TrackDetailProjection` and UIModel policies.

The detail scope owns a projection subscription and one undo controller for its descendant components.
The field grid and tag editor borrow that scope and create UIModel authoring sessions from its exact selected ids when an edit begins.
They do not construct storage, call `LibraryCommands`, or create a competing detail snapshot.

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
- Inline-session invalidation defers teardown until observer delivery unwinds; replacing or clearing the session disconnects that exact idle callback before releasing session state.
- Custom deletion undo is offered only after `Applied`, and only when every bound target has the same stored value, including a common present empty value. Mixed or missing values may still be deleted without an offer.
- Credits-clear undo is offered only after an `Applied` clear of one common non-empty scope. Mixed, empty, and absent scoped baselines have no clear-undo value; unrelated mixed kinds do not prevent an offer.
- Custom deletion and Credits clear are mutually exclusive variants of one undo opportunity. They share `track.detailUndoBar` and one five-second timer; a current offer of either kind displaces the other.
- A change of the detail scope's target-id sequence clears the pending opportunity. Timeout, successful or no-op replay, and a terminal replay failure clear only that opportunity. `Busy` does not clear it and leaves it available for retry. A non-current session cannot displace or resurrect an opportunity, and a retired replay or timer cannot clear a replacement.
- An overlapping custom edit of the same key clears a pending custom-delete undo. An overlapping Credits change clears a pending Credits-clear undo only when its kind mask overlaps and that offer's bound revision is less than or equal to the mutation revision, so a newer clear is not invalidated by an older completion.
- The field grid, tag flow, and cover slot report compressible horizontal minima and cannot widen the collapsible detail pane from content.
- Invalid UTF-8 is replaced for GTK/Pango display without rewriting stored bytes until the user commits an edit.

## State model

The field grid retains expanded/collapsed section state, show-empty state, generated row/editor objects, current snapshot, custom-add popover state, and one active-editor coordinator.
The undo controller retains one opportunity: either a custom-delete undo (key, common prior value, and the applied edit's next-revision `TrackAuthoringSession`) or a Credits-clear undo (typed scoped replacement and that session), plus one opportunity identity and a five-second timer.
The two variants are mutually exclusive. Presenting a current offer resets the other, advances the opportunity identity, and restarts the timer.

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

Credits editing retains its ordered draft rows, focus, and completion attachments during same-selection snapshot refreshes, including external credit changes.
A refresh updates authoring validity but never reseeds the open draft; stale Save is refused rather than silently rebound.
Adding/deleting/reordering/reclassifying rows, replacing the selection, canceling, or completing a save retires the row generation on a later main-loop turn; signals from those retired rows cannot update shifted draft indices.
Custom-key topology changes keep the Credits surface parented at its stable grid position.

Category preview buttons open a fixed one-kind editor; Full Credits opens all four kinds.
The same native adapter exposes name, role, delete, within-kind move buttons, and an all-kind-only kind selector.
Kind changes append to the destination section; completion follows the selected row kind, while role completion is global.
Mixed scopes begin unseeded and require explicit replacement intent. Clear is a separate action inside the visibly labelled scope, staged until Commit.
Validation marks invalid name rows and refuses Save. Row controls are keyboard-focusable and vertically arranged; display text ellipsizes with full tooltips and the host reports a compressible horizontal minimum.
The compact table keeps category columns as read-only first-name/count projections, including duplicate entries in the localized additional count; no scalar category edit survives.

Credits Edit binds targets and reads the owning baseline through one short-lived library snapshot.
`loadTrackCreditsEditorBaseline()` aggregates that snapshot, and `undoValueForClearedTrackCredits()` returns a typed clear-undo value only for one common non-empty scope.
Custom deletion binds the same way and walks each bound target's stored custom value in that snapshot. It does not ask UIModel for an undo value.
Common present values, including empty, are eligible. Mixed or missing values allow deletion without an offer.
Preparation releases the snapshot before native installation or asynchronous submission and retains the final current and submission guards; a commit after binding can still make the prepared session stale.

An `Applied` custom deletion publishes its offer only when a prior value was captured and the detail selection still matches those targets.
An `Applied` Credits clear publishes its offer only when the baseline helper returned a scoped replacement and the editor generation and target ids still match.
Either publication transfers the applied session, now holding the next-revision binding, into the shared pending undo state and restarts the five-second timer.
A non-current session is not published and cannot displace a newer opportunity.
Undo restores the captured custom value or selected Credits segments through that session.
An intervening effective commit makes the offered session stale, so replay cannot overwrite newer library state.
Selection replacement clears the bar. `Busy` reports temporary contention and leaves the opportunity in place. Stale, unavailable, and operational terminal failures clear that opportunity and publish an error notification; a completion for a displaced opportunity does not.

### Properties composition

Track Properties embeds the same native Credits editor against its parent `TrackPropertiesFormModel`.
Binding, ordinary field baselines, and per-kind Credits baselines are captured from the same snapshot, released before installing widgets.
Accepting a child updates only the form's pending scoped replacement; sequential scopes overlay the captured baseline without rereading the library.
An active child blocks parent Save, including programmatic dialog responses.
Cancel preserves previously accepted scopes and ordinary dirty fields; the parent submits one composed patch through its retained session.
Stale and Busy responses never rebind or reload a dirty draft.

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
Undo failure clears no committed library mutation.
A stale, unavailable, or operational terminal failure clears that opportunity and the undo bar publishes it as an error notification.
A `Busy` result reports temporary resource contention without clearing the pending undo.
A completion for a displaced opportunity does not clear its replacement.

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

- [`TrackDetailScope.cpp`](../../../../app/linux-gtk/layout/component/track/TrackDetailScope.cpp) owns snapshot and undo scope.
- [`TrackFieldGridComponent.cpp`](../../../../app/linux-gtk/layout/component/track/TrackFieldGridComponent.cpp) owns field-grid composition, commands, and the bound-snapshot custom-delete undo walk. It offers that walk's prior value only after `Applied`.
- [`TrackDetailUndo.h`](../../../../app/linux-gtk/layout/component/track/TrackDetailUndo.h) and [`TrackDetailUndo.cpp`](../../../../app/linux-gtk/layout/component/track/TrackDetailUndo.cpp) own the mutually exclusive custom-delete and Credits-clear opportunity, its identity, and the five-second timer.
- [`TrackDetailUndoBarComponent.cpp`](../../../../app/linux-gtk/layout/component/track/TrackDetailUndoBarComponent.cpp) renders the shared bar for whichever variant is pending.
- [`TrackCreditsEditor.cpp`](../../../../app/linux-gtk/layout/component/track/TrackCreditsEditor.cpp) owns live scoped draft controls, completion attachments, detail submission, and the Credits-clear offer.
- [`TrackCredits.h`](../../../../app/include/ao/uimodel/library/detail/TrackCredits.h) owns the shared editor, scoped baseline, display, and clear-undo eligibility.
- [`TrackPropertiesDialog.cpp`](../../../../app/linux-gtk/tag/TrackPropertiesDialog.cpp) owns the form binding and atomic parent Save.
- [`TrackRowCache.cpp`](../../../../app/linux-gtk/track/TrackRowCache.cpp) and [`TrackRowObject.cpp`](../../../../app/linux-gtk/track/TrackRowObject.cpp) retain compact first-name/count rows, not full credit lists.
- [`TrackAuthoringSessions.h`](../../../../app/include/ao/uimodel/library/track/TrackAuthoringSessions.h) owns the target/revision binding used by GTK editors and undo.
- [`TagEditor.cpp`](../../../../app/linux-gtk/tag/TagEditor.cpp) owns chip flow and add/search interaction.
- [`TrackTagEditorComponent.cpp`](../../../../app/linux-gtk/layout/component/track/TrackTagEditorComponent.cpp) binds tag editor to detail scope.
- [`TrackCoverArtComponent.cpp`](../../../../app/linux-gtk/layout/component/track/TrackCoverArtComponent.cpp) owns cover-slot adaptation.

## Test-only settlement observation

The source-private [`TrackFieldGridOperationProbe`](../../../../app/linux-gtk/layout/component/track/TrackFieldGridOperationProbe.h) observes the field grid's live metadata task scope on its GTK owner executor.
It includes built-in and custom metadata submissions, but not the nested Credits editor or shared Undo controller.
After observing admission, a transition to empty with the owner still alive and no cancellation witnesses terminal frontend completion.
An empty queue, a durable commit, or an empty scope after cancellation does not establish that boundary.
The probe retains nothing and schedules no work; its CMake boundary rejects production consumers outside the field-grid implementation and declaration.

## Test map

- [`TrackFieldGridCollapsibleTest.cpp`](../../../../test/unit/linux-gtk/layout/components/TrackFieldGridCollapsibleTest.cpp) protects sections and empty-field visibility.
- [`TrackDetailConstrainedLayoutTest.cpp`](../../../../test/unit/linux-gtk/layout/components/TrackDetailConstrainedLayoutTest.cpp) protects stable rows, clipping, anchors, and narrow allocation.
- [`TrackFieldGridTextTest.cpp`](../../../../test/unit/linux-gtk/layout/components/TrackFieldGridTextTest.cpp) protects mixed/unknown/display text.
- [`SemanticLayoutComponentsTest.cpp`](../../../../test/unit/linux-gtk/layout/components/SemanticLayoutComponentsTest.cpp) protects scope, mutations, shell binding, the shared undo bar, five-second timeout, stale and rejected terminal replay, destruction during replay, selection clear, common present-empty eligibility, and refusal of a stale session to displace a current opportunity. Its mixed and missing deletion cases observe admitted-to-terminal field-grid submission before checking stored deletion and absence of an undo offer.
- [`TrackCreditsRefreshTest.cpp`](../../../../test/unit/linux-gtk/layout/components/TrackCreditsRefreshTest.cpp) protects same-selection input, focus, completion, custom-key topology, pending-publication settlement, and stale Save; [`TrackCreditsEditorTest.cpp`](../../../../test/unit/linux-gtk/layout/components/TrackCreditsEditorTest.cpp) protects scoped controls, Properties staging, row retirement, selection replacement, owner teardown, coherent clear-undo, and opportunity identity for retired replay, timeout, and overlapping scoped changes.
- [`TrackCreditsTest.cpp`](../../../../test/unit/uimodel/library/detail/TrackCreditsTest.cpp) protects scoped editor and clear-undo policy.
- [`TagEditorTest.cpp`](../../../../test/unit/linux-gtk/tag/TagEditorTest.cpp) and [`TagEditControllerTest.cpp`](../../../../test/unit/linux-gtk/tag/TagEditControllerTest.cpp) protect chip flow and edit interaction.
- [`TrackColumnFactoryBuilderTest.cpp`](../../../../test/unit/linux-gtk/track/TrackColumnFactoryBuilderTest.cpp) protects inline-session invalidation, deferred teardown cancellation, and recycled-cell editing.

## Related documents

- [Presentation architecture](../../presentation/README.md)
- [Application shell architecture](../../shell/README.md)
- [Metadata-editing specification](../../presentation/metadata-editing.md)
- [Shell layout adaptation](../../shell/layout-adaptation.md)
- [Cover-art resource delivery](../../resource/cover-art-delivery.md)
- [GTK layout schema reference](../../../reference/shell/layout-schema.md)
