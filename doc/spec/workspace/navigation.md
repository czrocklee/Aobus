---
id: workspace.navigation
type: spec
status: current
domain: workspace
summary: Defines validated workspace commands, revisioned snapshots, semantic history, and coherent observation.
---
# Workspace navigation

## Scope

This specification defines current frontend-neutral workspace navigation behavior.
It owns the open and focused view aggregate, semantic target resolution, history commit and replay, custom-preset membership, command results, and workspace observations.

It does not define source membership, projection rows, presentation rendering, persisted session fields, or frontend input bindings.
Those facts belong to the linked library, presentation, workspace-session, reference, and frontend owners.

## Code boundary

This contract belongs to the **application runtime** layer under the [workspace architecture](../../architecture/workspace.md).
Its public boundary is `WorkspaceService.h`, `WorkspaceSnapshot.h`, `NavigationHistory.h`, and the view values consumed from `ViewService`; its implementation is `WorkspaceService.cpp` and `NavigationHistory.cpp`.

`WorkspaceService` depends on `ViewService`, committed `LibraryChanges`, presentation values, and the runtime callback executor.
It does not depend on playback.
`AppRuntime::jumpToAlbum()` owns the one current command that composes workspace navigation with playback reveal.

## Terminology

- **Workspace snapshot** is the complete revisioned value containing open views, focus, and custom presets.
- **Navigation target** is a base list, a base list plus filter expression, or a supported global-view kind.
- **Navigation request** combines one target, history-recording policy, and an optional presentation intent.
- **Presentation intent** is either `Override`, which applies to reused and new views, or `NewViewDefault`, which applies only when navigation creates a view.
- **Navigation point** is `{listId, filterExpression, presentation}` stored in transient history.
- **Command result** carries only a value needed by the caller; navigation returns its active `ViewId`, presentation lookup returns its resolved specification, and other mutations return empty success.
- **Workspace change** is one post-commit observation containing a complete snapshot and a typed cause.

## Invariants

- Every id in `openViews` is nonzero, unique, live, and owned by the bound `ViewService`.
- `activeViewId` is invalid when no view is focused; otherwise it occurs exactly once in `openViews`.
- One accepted semantic transition advances the workspace revision exactly once and publishes one `WorkspaceChanged` value.
- A successful no-op preserves the revision and publishes nothing.
- Recoverable command failure preserves the workspace snapshot and live history cursor.
- A navigation point stores semantic reconstruction input, never `ViewId`, selection, scroll position, widget state, or a projection handle.
- Committing a point equal to the current point is a no-op; committing after backward traversal removes forward points.
- History contains at most 256 points and is not persisted.
- Quick-filter editing remains view-owned and does not enter history unless a workspace navigation command explicitly commits the resulting semantic point.

## State model

`WorkspaceSnapshot` contains:

- ordered `openViews`;
- `activeViewId`;
- the complete custom-preset collection; and
- a monotonically increasing workspace-local `revision`.

Each view separately owns `TrackListViewState`, including list, filter, expression error, presentation, and selection.
The workspace snapshot identifies those owners without duplicating their complete state.

Invalid input and preparation failure use `Result` errors.

## Commands and transitions

### Navigate to a target

`navigate(request)` resolves and prepares the request's target before changing the aggregate.

- A plain `ListId` reuses an existing live, unfiltered view for that list when one exists.
- A plain list without a reusable view creates a view with an empty filter.
- A filtered-list target creates a distinct view from its explicit list and filter.
- `GlobalViewKind::AllTracks` resolves to the All Tracks virtual list; unsupported global values return `InvalidInput`.

`WorkspaceService` is the sole authority for the reuse-or-create decision.
Presentation intent follows this matrix:

| Request presentation | Reused view | Newly created view |
|---|---|---|
| Absent | Preserve the exact current presentation. | Use the `ViewService` default. |
| `Override` | Normalize and apply the supplied specification. | Normalize and apply the supplied specification. |
| `NewViewDefault` | Ignore the supplied specification and preserve the exact current presentation. | Normalize and apply the supplied specification. |

A filtered target is never reusable, so a supplied `NewViewDefault` applies to its newly created view.
When the request records history, its navigation point stores the final effective presentation after this decision.

The command then prepares the next open set, focus, and history candidate and commits them once.
Navigation returns the resulting active `ViewId`.
Repeating navigation to the already-active equivalent point returns that same id without advancing revision or publishing.
A missing list, source, or failed presentation update returns the lower recoverable error without changing snapshot or history.

### Focus an open view

`focusView(viewId)` accepts only a live view already present in `openViews`.
Invalid, unknown, and unopened ids are rejected.
Focusing the active view succeeds without publication; a different open view produces one `Focus` commit and does not implicitly append history.

There is no public raw add-view operation.
Opening and adopting a view is part of semantic navigation or session restoration.

### Change active presentation

`setActivePresentation(spec, presentationChangeOptions)` requires an active live view, normalizes the complete specification, applies it through `ViewService`, and optionally commits the resulting navigation point.
Presentation-change options contain only the history-recording policy and cannot carry navigation target or presentation-mode fields.
The complete presentation value participates in equality, including visible and redundant fields.

The id overload resolves built-in presets and then workspace custom presets.
It returns the resolved presentation.
An unknown id returns `NotFound`; a missing active view returns `InvalidState`.
Reapplying an identical presentation and history point succeeds without publication.

### Traverse history

`goBack()` and `goForward()` copy the history candidate and move only that candidate cursor.
Replay finds a live view matching list and filter or creates a replacement, restores the exact presentation, prepares the next open/focused snapshot, and commits snapshot plus cursor once.
Replay never appends a history point.

Traversal past an end returns `NotFound`.
Failed target recreation or presentation restoration leaves the live cursor, snapshot, and revision unchanged.

### Close views and delete lists

`closeView(viewId)` removes an open view, selects the last remaining open view as fallback when necessary, commits once, and releases the view through `ViewService`.
Closing an id not present in the workspace succeeds without publication and does not call view destruction.

A committed library-list deletion closes every matching open view in one `ListDeletion` commit, even when several filtered views use the same base list.
History points remain semantic snapshots; replaying a point for a deleted list fails during reconstruction.

### Edit custom presets

Adding a new preset or changing the preset with the same label produces one `Presets` commit.
Adding an identical value or removing an absent id succeeds without publication.
Removing an existing preset likewise commits one complete preset collection.
Stable preset identity and versioned persistence remain owned by the presentation and workspace-session proposals.

### Reveal an album

Album reveal is not a `WorkspaceService` command.
`AppRuntime::jumpToAlbum(trackId)` rejects the invalid track id, navigates to All Tracks with an explicit `Override` carrying the built-in albums presentation, and submits playback reveal for the returned active view.
The command returns empty success after submitting reveal.
Workspace navigation remains committed if a future reveal boundary gains a separate asynchronous failure after submission.

## Preparation and commit

Commands copy the small workspace snapshot and bounded history before installing either value.
View creation and presentation updates complete before the no-fail aggregate swap.
`ViewService::createView()` itself publishes no creation signal, so a synchronously prepared view is not exposed as a partial workspace aggregate.

Session restore creates every view first and destroys all created candidates if a later creation fails.
Successful restore installs all ids, focus, presets, and one revision together.
The implementation deliberately does not add a generic transaction object, detached candidate lease, command generation, or asynchronous command state machine.

## Executor, observation, and lifetime

Workspace reads, subscriptions, and commands are confined to the runtime callback executor.
An off-executor call is a programming error and fails fast rather than racing mutable state.
Commands are bounded and synchronous and expose no cancellation point.

`snapshot()` returns the current value by copy.
`onChanged()` delivers `WorkspaceChanged{snapshot, cause}` on a later callback-executor turn.
The event owns its snapshot, so a command called by an observer cannot mutate the event currently being delivered; its own observation is queued separately.

One throwing observer does not starve later observers and cannot change an already-returned command result.
The failure is contained and logged at the workspace publication boundary.
Queued delivery holds only a weak signal owner, so destruction makes undelivered events harmless.

A newly constructed consumer reads `snapshot()` before subscribing or records that revision and ignores any queued event that is not newer.
Test-only `InlineExecutor` deliberately collapses turns when deferred-turn behavior is outside a test; observation-order tests use the queued executor.

## Persistence

Navigation history is transient.
Workspace session restore commits the restored active view as the initial navigation point, so Back remains unavailable until a later distinct navigation.
The [workspace session specification](session.md) owns capture and restore, and the [workspace session state reference](../../reference/workspace/session-state.md) owns serialized fields.

## Implementation map

- [`WorkspaceService`](../../../app/include/ao/rt/WorkspaceService.h) defines semantic commands and observations.
- [`WorkspaceSnapshot`](../../../app/include/ao/rt/WorkspaceSnapshot.h) defines snapshots and change causes.
- [`NavigationHistory`](../../../app/include/ao/rt/NavigationHistory.h) owns bounded semantic history.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) owns view-local state, projections, and resources.
- [`AppRuntime`](../../../app/include/ao/rt/AppRuntime.h) owns album reveal composition above workspace and playback.

## Test map

- [`NavigationHistoryTest.cpp`](../../../test/unit/runtime/NavigationHistoryTest.cpp) proves cursor and capacity mechanics.
- [`WorkspaceNavigationTest.cpp`](../../../test/unit/runtime/WorkspaceNavigationTest.cpp) proves validation, command results, complete observations, reentrancy, observer containment, deletion batching, and album reveal.
- [`WorkspaceHistoryTest.cpp`](../../../test/unit/runtime/WorkspaceHistoryTest.cpp) proves atomic history replay.
- [`WorkspacePresentationTest.cpp`](../../../test/unit/runtime/WorkspacePresentationTest.cpp) proves presentation and preset commands.
- [`WorkspaceSessionTest.cpp`](../../../test/unit/runtime/WorkspaceSessionTest.cpp) proves atomic multi-view restore and failure cleanup.
- [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) proves frontend-neutral reconstruction.

## Related documents

- [Workspace architecture](../../architecture/workspace.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Workspace session](session.md)
- [Workspace session state](../../reference/workspace/session-state.md)
- [Track filtering](../presentation/track-filter.md)
- [Track-list presentation](../presentation/track-presentation.md)
