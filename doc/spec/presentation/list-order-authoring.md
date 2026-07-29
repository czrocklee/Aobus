---
id: presentation.list-order-authoring
type: spec
status: current
domain: presentation
summary: Defines saved-List order eligibility, stable-ID gestures, authoring-session invalidation, GTK drag behavior, and keyboard movement.
---
# Saved-List order authoring

## Scope

This specification defines when an interactive view may modify saved List rank and how drag, context-menu, and keyboard gestures become stable-ID order commands.
It owns capability derivation, selection normalization, absolute and relative movement intent, authoring-session lifetime, user-visible unavailability, GTK generation ownership, and keyboard repeat behavior.

Raw/effective rank semantics and writer transactions belong to [track sources](../library/source/track-source.md) and [library mutation](../library/runtime/mutation.md).
Presentation sorting belongs to [track-list presentation](track-presentation.md).
Exact default chords belong to the [keyboard map](../../reference/shell/keymap.md).

## Code boundary

Platform-neutral capability and session policy lives in UIModel under `app/include/ao/uimodel/library/list/` and `app/uimodel/library/list/`.
It consumes runtime view, projection, library-availability, and authoring ports without depending on GTK.

GTK owns `TrackOrderDragController`, native row/drop controllers, autoscroll, indicators, status rendering, selection adaptation, and shortcut-event suppression.
It does not submit row indexes to storage or call a core store directly.

## Terminology

- **Base capability** means the active view may write this saved List's order.
- **Gap move** inserts selected identities at a gap in the complete effective sequence.
- **Relative move** moves selected identities one unselected position up or down.
- **Absolute move** moves selected identities to the complete effective sequence's top or bottom.
- **Order binding** is runtime instance, committed revision, List id, and complete effective source TrackId sequence.
- **Gesture generation** is the active view/projection/widget generation used only to cancel stale interaction early.

## Invariants

- The virtual All Tracks source never has order-authoring capability.
- Capability depends on presentation structure, not presentation id: `groupBy` must be `None` and `sortBy` must be empty.
- Visible fields and whether raw rank is currently empty do not affect capability.
- Source state must be live and error-free, and library authoring must be `Available`.
- A quick filter disables gap and relative moves but leaves absolute moves, Reset Order, and Forget Hidden Positions available.
- Movement operands and anchors are stable TrackIds; GTK row positions never cross the runtime authoring boundary.
- A dragged selected row moves the complete selection in effective source order.
- A dragged unselected row becomes the sole dragged selection.
- One effective movement is one semantic writer command and one library commit.
- A no-op writes nothing and may keep its authoring session current.
- A non-no-op result consumes the session; a later move starts from a new binding.
- Frontend generation is cancellation evidence, not writer transaction authority.

## State model

`describeListOrderCapabilities()` yields independent flags for:

- general order authoring;
- gap movement;
- relative movement;
- absolute movement;
- Reset Order;
- Forget Hidden Positions;
- one user-facing disabled reason.

Evaluation order makes the most actionable blocking state visible:

1. saved List rather than a virtual source;
2. maintenance or other authoring availability;
3. live, error-free source;
4. flat presentation with empty sort;
5. quick-filter restriction for gap/relative movement.

Maintenance produces **Library is busy. Manual ordering will be available when maintenance finishes.**
GTK displays that state in the track-page status surface rather than silently omitting the interaction.

`ListOrderAuthoringSession` owns one `BoundListOrder`, the matching projection, capability snapshot, and subscriptions.
It becomes invalid when any of these occurs:

- authoring leaves `Available`, runtime identity changes, or committed revision changes;
- presentation changes;
- the view replaces its projection, including a quick-filter change;
- the projection publishes a membership/order delta;
- the view is destroyed;
- a submitted command returns any status other than `NoOp`.

Invalidation clears every capability and notifies the current gesture.
The session does not silently rebind to newer rows.

## Commands and transitions

### Binding

`begin(library, views, viewId)` reads one coherent view state, describes capability, obtains the complete effective saved-List source sequence, asks runtime for a revision-bound order binding, and retains the current projection.
Failure returns a typed error with the capability reason where applicable.

The bound sequence is the complete base List sequence, not only quick-filter-visible projection rows.
That distinction makes top/bottom movement unambiguous while a quick filter is active.

### Drag selection and gap

At drag start, GTK resolves current selected TrackIds.
If the dragged row was not selected, it selects that row and moves only it.
Otherwise UIModel intersects the selection with the bound effective sequence and returns the selected IDs in sequence order.

For a visible gap, UIModel scans forward from that gap to find the next unselected effective TrackId.
That identity becomes `beforeTrackId`; absence means the end.
Selected rows are skipped, so dropping beside the moving block normalizes to the next meaningful anchor and may become a writer no-op.

GTK accepts only the opaque token created by the same view-local drag.
The upper or lower half of a row denotes its before or after gap.
Approaching the top or bottom 36 logical pixels advances the vertical adjustment by a bounded step.
The indicator is cleared on leave, unbind, invalidation, drag end, submission, or controller close.

### Relative and absolute movement

Move Up and Move Down remove the selected identities conceptually, locate the first/last selected position among remaining identities, and shift the block by one unselected position.
Selection order always comes from the bound effective source.

Move to Top supplies the first complete-sequence insertion point.
Move to Bottom supplies no anchor.
These absolute commands remain available through a quick filter because their target is defined against the complete sequence.

Reset Order and Forget Hidden Positions use the same binding and follow the mutation semantics owned by the library specification.
They do not reinterpret quick-filter-hidden rows as List-nonmembers.

### Keyboard dispatch

The shipped bindings are:

- `Alt+Up`: Move Up;
- `Alt+Down`: Move Down;
- `Alt+Home`: Move to Top;
- `Alt+End`: Move to Bottom.

Order movement is intentionally one command per physical key-down/key-up cycle.
GTK's capture-phase guard keys by physical keycode and consumes repeated press events until release.
Window deactivation and keymap replacement reset the guard.
Holding a key therefore moves once; another committed step requires release and a new press.

Reset and Forget Hidden Positions remain menu/action commands without shipped global shortcuts.

## Failure and cancellation

Writer results are `Applied`, `NoOp`, `Stale`, or `Unavailable`.
Applied and NoOp receive explicit success/status text.
Stale tells the user that the List changed and to start again.
Unavailable reports that editing is currently unavailable; maintenance has the more specific library-busy reason before submission.

A source error, grouped/sorted presentation, active quick filter for a relative action, missing selection, or destroyed view disables the command with its current reason.
GTK never clears a quick filter, changes presentation, or prunes hidden positions as an implicit workaround.

Drag callbacks hold weak/shared generation-local state.
On ColumnView rebuild or page destruction, GTK first closes and destroys `TrackOrderDragController`, detaches the model and widget tree, and only then installs replacement-generation controllers.
Late callbacks observe `closing`, an invalid token, or expired state and cannot submit against a new view generation.

## Persistence and versioning

The gesture/session is transient.
Only the successful runtime mutation persists rank in the List record.
Default shortcut ids and chords follow the keyboard-map compatibility policy; order semantics follow the library database version.
There is no persisted drag, provisional order, gesture-generation, or repeat-guard state.

## Frontend observations

GTK prepends a 36-pixel drag-handle column only when gap movement is available.
The right-click **Manual Order** submenu exposes relative, absolute, Reset, and Forget Hidden commands according to their separate flags and shows the blocking reason when order authoring is unavailable.
With a quick filter, the submenu keeps Move Up and Move Down visibly disabled beside the relative-movement reason while Move to Top, Move to Bottom, Reset Order, and Forget Hidden Positions remain available.

Other frontends may adapt the same UIModel capabilities to native interactions.
They must preserve stable-ID operands, complete-sequence semantics, revision-bound submission, and explicit unavailable states.

## Implementation map

- [`ListOrderPolicy.h`](../../../app/include/ao/uimodel/library/list/ListOrderPolicy.h) defines capability and gap/selection normalization.
- [`ListOrderAuthoringSession.h`](../../../app/include/ao/uimodel/library/list/ListOrderAuthoringSession.h) defines binding and invalidation ownership.
- [`KeyRepeatGuard.h`](../../../app/include/ao/uimodel/input/KeyRepeatGuard.h) defines one-press-per-physical-key-cycle policy.
- [`TrackOrderDragController.cpp`](../../../app/linux-gtk/track/TrackOrderDragController.cpp) owns the GTK generation-local DnD surface.
- [`TrackViewPage.cpp`](../../../app/linux-gtk/track/TrackViewPage.cpp) adapts capabilities and order commands.
- [`MainWindow.cpp`](../../../app/linux-gtk/app/MainWindow.cpp) suppresses native order-key auto-repeat.

## Test map

- [`ListOrderPolicyTest.cpp`](../../../test/unit/uimodel/library/list/ListOrderPolicyTest.cpp) protects the capability matrix, including quick-filter and maintenance reasons, and selection/gap normalization.
- [`ListOrderAuthoringSessionTest.cpp`](../../../test/unit/uimodel/library/list/ListOrderAuthoringSessionTest.cpp) protects binding, movement, and invalidation.
- [`LibraryAuthoringTest.cpp`](../../../test/unit/runtime/library/LibraryAuthoringTest.cpp) protects maintenance admission, including rejection of List-order binding while authoring is unavailable.
- [`KeyRepeatGuardTest.cpp`](../../../test/unit/uimodel/input/KeyRepeatGuardTest.cpp) protects physical-key repeat suppression.
- [`TrackViewPageTest.cpp`](../../../test/unit/linux-gtk/track/TrackViewPageTest.cpp) protects GTK eligibility and command adaptation.
- [`TagEditControllerTest.cpp`](../../../test/unit/linux-gtk/tag/TagEditControllerTest.cpp) protects the visible Manual Order menu capability split.
- [`TrackSelectionControllerTest.cpp`](../../../test/unit/linux-gtk/track/TrackSelectionControllerTest.cpp) protects the blank-area right-click no-menu contract.

## Related documents

- [List model](../../reference/library/model/list.md)
- [Track sources](../library/source/track-source.md)
- [Library mutation](../library/runtime/mutation.md)
- [Track-list presentation](track-presentation.md)
- [Keyboard map](../../reference/shell/keymap.md)
- [Organize music with Lists and Playlists](../../user/organize-with-lists.md)
