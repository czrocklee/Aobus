---
id: workspace.navigation
type: spec
status: current
domain: workspace
summary: Defines workspace view navigation, semantic history points, traversal, focus, closure, and observable updates.
---
# Workspace navigation

## Scope

This specification defines current frontend-neutral workspace navigation behavior.
It owns target resolution, open and focused view transitions, navigation-point commit and replay, back/forward traversal, view closure caused by user intent or list deletion, and the resulting workspace observations.

It does not define source membership, projection row behavior, presentation grouping and sorting, session serialization, or frontend input bindings.
Those facts belong to the library, presentation, workspace-session, and frontend owners linked below.

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../architecture/system-overview.md), under the ownership graph defined by the [workspace architecture](../../architecture/workspace.md).
Its public boundary is `app/include/ao/rt/WorkspaceService.h`, `NavigationHistory.h`, `WorkspaceViewState.h`, and the view values consumed from `ViewService`; its implementation is `app/runtime/WorkspaceService.cpp` and `NavigationHistory.cpp`.

Workspace may use runtime library sources, presentation values, and a narrow playback reveal command without depending on UIModel or frontends.
The [library architecture](../../architecture/library.md) owns source and projection construction, while the [presentation architecture](../../architecture/presentation.md) owns outward display adaptation.

## Terminology

- **Workspace** is the canonical aggregate of open runtime views and one focused view.
- **View** is a runtime-local state and projection owner identified by `ViewId`.
- **Navigation target** is a base list, a base list plus filter expression, or a supported global-view kind.
- **Navigation point** is the semantic snapshot `{listId, filterExpression, presentation}` stored in history.
- **Commit** appends or deduplicates the final active navigation point.
- **Replay** restores a history point during backward or forward traversal without committing another point.

## Invariants

- A navigation point never stores `ViewId`, selection, scroll position, widget layout, sidebar state, or a source/projection handle.
- A navigation point contains enough semantic state to recreate the track-list view visible at commit time: base `ListId`, filter expression, and exact `TrackPresentationSpec`.
- Navigation commits the final result of a composite command once, after its view, filter, presentation, focus, and reveal steps have completed.
- Replay never grows history or changes the meaning of an existing history point.
- Committing a point equal to the current point is a no-op.
- Committing after backward traversal removes all forward points before appending the new point.
- History contains at most 256 points in normal workspace composition; capacity eviction removes the oldest point.
- Navigation history is not persisted as workspace session state.
- A failed target resolution or failed replay leaves the workspace aggregate and history cursor unchanged.
- Presentation changes recorded by workspace navigation do not change list membership.
- Quick-filter text editing does not commit history by itself; a command that wants a filtered navigation point must perform or request a commit explicitly.

## State model

`WorkspaceViewState` contains:

- `openViews`, in workspace ordering;
- `activeViewId`, or the invalid view id when no view is focused;
- a monotonically increasing workspace-local `revision` for aggregate updates.

`NavigationHistory` contains a bounded sequence of navigation points and an optional current index.
An empty history has no current point and supports neither direction.

Each view separately owns its `TrackListViewState`, including list, filter, presentation, selection, lifecycle, and view-local revision.
Workspace state refers to those views but does not duplicate their complete state.

## Commands and transitions

### Navigate to a target

`navigateTo(target, options)` resolves the target before changing workspace focus.

- A plain `ListId` reuses an existing non-destroyed, unfiltered track-list view for that list when one exists.
- When a reused plain-list view receives `optPresentation`, that presentation is installed before the view is focused.
- A plain list without a reusable view creates an attached view with an empty filter.
- A filtered-list target creates a new attached view from its explicit base list and filter expression; current behavior does not search for an equivalent filtered view.
- `GlobalViewKind::AllTracks` resolves to the All Tracks virtual list.
- Any other global-view enum value is rejected as invalid input.

After resolution succeeds, workspace adds the view to `openViews` when necessary, makes it active, increments the workspace revision, emits focus change, and commits the resulting point unless `recordHistory` is false or replay is active.

### Focus and open-set helpers

`addView(viewId)` adds an absent id and increments the workspace revision; adding a duplicate is a no-op.
`setFocusedView(viewId)` installs the supplied id, increments the revision, and emits focus change.
These low-level helpers rely on the caller to supply a view owned by the same live `ViewService`; they do not validate membership themselves.
[RFC 0016](../../rfc/0016-coherent-workspace-transactions.md) proposes replacing them with validated result-bearing semantic commands and one atomic aggregate commit.

### Change active presentation

Changing the active presentation updates the focused view through `ViewService` and then commits the final navigation point unless history recording is disabled.
With no focused view, the command has no effect.

The id-based command resolves built-in presets first and custom workspace presets second.
An unknown id or failed runtime update produces an empty returned spec or no state change on the current void overload.
Presentation resolution and display semantics belong to the [track-list presentation specification](../presentation/track-presentation.md).

### Jump to album

`jumpToAlbum(trackId)` is one composite navigation command.
For a valid track id it resolves or creates an unfiltered All Tracks view, focuses it, installs the `albums` presentation when that preset exists, requests playback reveal for the track, and commits the final point once.
An invalid track id is a no-op.

### Traverse history

`goBack()` and `goForward()` first move a tentative history cursor and then restore the selected point.
Replay searches for a non-destroyed view matching both list and filter.
It creates an attached replacement view when no match exists, otherwise installs the point's exact presentation on the matching view.

Successful replay opens and focuses the resolved view, increments the workspace revision, emits focus change, and emits current back/forward availability.
Replay does not commit a new point.

If restoration fails, traversal restores the previous history container before returning the error.
The prior open-view set, focus, workspace revision, and directional availability remain unchanged.

### Close a view

Closing removes the id from `openViews`.
If it was active, the last remaining open view becomes active; an empty workspace uses the invalid view id.
Workspace increments its revision, emits the resulting focused id, and asks `ViewService` to destroy the view.

Committed deletion of a library list closes every open workspace view whose base list is that list.
History points remain semantic snapshots; attempting to replay a point for a deleted list fails rather than silently changing its target.

## Failure and cancellation

Navigation and replay return recoverable `Result<ViewId>` values.
An unsupported target returns `InvalidInput`; an absent list or source returns the lower `NotFound`-class failure; traversal past either end returns `NotFound`.

Target preparation completes before focus and history mutation.
Replay copies the history container before moving its cursor so a failed view recreation can roll the cursor back.

The workspace logs failures from current void presentation and destroy paths rather than returning them to those callers.
Cross-application reporting policy is owned by the [failure and reporting architecture](../../architecture/failure-and-reporting.md), and proposed result-bearing convergence is tracked outside this current specification.
[RFC 0016](../../rfc/0016-coherent-workspace-transactions.md) proposes the workspace-specific command, receipt, observation, and reentrancy boundary.

Workspace commands are synchronous on the runtime callback executor and expose no cancellation point.

## Persistence and versioning

Navigation history is intentionally transient.
Workspace session restore may recreate open and focused views, then commits only the restored active view as the initial history point.
Back remains unavailable until a later navigation commits a second distinct point.

The [workspace session specification](session.md) owns save and restore behavior, and the [workspace session state reference](../../reference/workspace/session-state.md) owns exact serialized fields.

## Frontend observations

`layoutState()` returns the current workspace aggregate by value.
`onFocusedViewChanged` emits the resulting active id after focus, replay, or closure transitions.

`onNavigationHistoryChanged` publishes only `{canGoBack, canGoForward}`.
A normal commit emits it when either availability value changes; a successful backward or forward replay emits the current values after traversal.
A deduplicated commit that does not change availability emits nothing.

GTK maps mouse back/forward buttons to traversal.
TUI currently uses workspace navigation when opening lists but owns its terminal-specific input and overlay behavior separately.

## Implementation map

- [`WorkspaceService`](../../../app/include/ao/rt/WorkspaceService.h) defines navigation targets, options, commands, and observations.
- [`WorkspaceService.cpp`](../../../app/runtime/WorkspaceService.cpp) owns target resolution, focus, composite navigation, replay, and list-deletion handling.
- [`NavigationHistory`](../../../app/include/ao/rt/NavigationHistory.h) and [`NavigationHistory.cpp`](../../../app/runtime/NavigationHistory.cpp) own bounded commit and cursor mechanics.
- [`WorkspaceViewState`](../../../app/include/ao/rt/WorkspaceViewState.h) defines the aggregate snapshot.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) owns the views and projections that workspace coordinates.

## Test map

- [`NavigationHistoryTest.cpp`](../../../test/unit/runtime/NavigationHistoryTest.cpp) proves empty state, commit, deduplication, traversal, forward truncation, and capacity eviction.
- [`WorkspaceNavigationTest.cpp`](../../../test/unit/runtime/WorkspaceNavigationTest.cpp) proves target resolution, focus, global and filtered targets, album jump, list deletion, and failure atomicity.
- [`WorkspaceHistoryTest.cpp`](../../../test/unit/runtime/WorkspaceHistoryTest.cpp) proves semantic replay, presentation restoration, destroyed-view recreation, observations, and traversal rollback.
- [`WorkspacePresentationTest.cpp`](../../../test/unit/runtime/WorkspacePresentationTest.cpp) proves active-presentation commits, suppression, resolution, and custom preset use.
- [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) proves open/focused aggregate behavior without a frontend.

## Related documents

- [Workspace architecture](../../architecture/workspace.md)
- [Library architecture](../../architecture/library.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Workspace session](session.md)
- [Workspace session state](../../reference/workspace/session-state.md)
- [Track filtering](../presentation/track-filter.md)
- [Track-list presentation](../presentation/track-presentation.md)
- [List presentation preference](../presentation/list-preference.md)
