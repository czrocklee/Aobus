---
id: workspace.navigation
---

# Workspace navigation

## Scope

This specification defines current frontend-neutral workspace navigation behavior.
It owns the open and focused view aggregate, semantic target resolution, history commit and replay, custom-preset membership, command results, and workspace observations.

It does not define source membership, projection rows, presentation rendering, persisted session fields, or frontend input bindings.
Those facts belong to the linked library, presentation, workspace-session, reference, and frontend owners.

## Code boundary

This contract belongs to the **application runtime** layer under the [workspace architecture](README.md).
Its public boundary is `WorkspaceService.h`, `WorkspaceSnapshot.h`, `NavigationHistory.h`, and the view values consumed from `ViewService`; its implementation is `WorkspaceService.cpp` and `NavigationHistory.cpp`.

`WorkspaceService` depends on `ViewService`, committed `LibraryChanges`, presentation values, and the runtime callback executor.
It does not depend on playback.
GTK, TUI, and WinUI pass the track and source List from a playback reveal request into workspace navigation. [Album reveal](#reveal-an-album) instead names an explicit destination view.

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
- Quick-filter editing and column sorting do not themselves change workspace history. A later recorded navigation can capture the edited view state.

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

- A plain `ListId` reuses a live, unfiltered view already open in the workspace for that list when one exists.
- A plain list without a reusable view creates a view with an empty filter.
- A filtered-list target creates a distinct view from its explicit list and filter.
- `GlobalViewKind::AllTracks` uses the same plain navigation rules for the All Tracks virtual list; unsupported global values return `InvalidInput`.

`WorkspaceService` is the sole authority for the reuse-or-create decision.
Presentation intent follows this matrix:

| Request presentation | Reused view                                                                    | Newly created view                              |
| -------------------- | ------------------------------------------------------------------------------ | ----------------------------------------------- |
| Absent               | Preserve the exact current presentation.                                       | Use the `ViewService` default.                  |
| `Override`           | Normalize and apply the supplied specification.                                | Normalize and apply the supplied specification. |
| `NewViewDefault`     | Ignore the supplied specification and preserve the exact current presentation. | Normalize and apply the supplied specification. |

A filtered target is never reusable, so a supplied `NewViewDefault` applies to its newly created view.
When the request records history, its destination point stores the final effective presentation after this decision.
A newly edited quick filter can form a distinct departure point when the current history entry names the same List with a different filter.

The command then prepares the next open set, focus, and history candidate and commits them once.
Navigation returns the resulting active `ViewId`.
Repeating navigation to the already-active equivalent point returns that same id without advancing revision or publishing.
A missing list, source, or failed presentation update returns the lower recoverable error without changing snapshot or history.

### Navigate to the playback source

`navigateToTrackSource` resolves **List membership before view visibility**. It uses the supplied source List if it still contains the track, even if another List on screen also contains it. An explicit All Tracks source, an unspecified source (`kInvalidListId`), a missing source List, or a source List that no longer contains the track resolves to All Tracks. If the All Tracks lookup does not find the track, the command returns `NotFound`.

A saved List whose filter cannot be evaluated is not a List that merely lacks the track. For example, an invalid inherited filter returns `FormatRejected` rather than falling back to All Tracks. Other lookup errors likewise propagate without changing the workspace or its history.

If the active view belongs to the resolved List and shows the track, navigation returns that view without publishing or recording history.
Otherwise, navigation focuses a live, unfiltered view already open for the resolved List or opens a new view with no quick filter.
An inactive filtered view is not reused, even if it shows the track.
Navigation leaves existing quick filters and the saved List unchanged.
Only a newly opened view consults the host's presentation default.

On a view change, workspace commits the live departure point and destination together. One Back restores the departing filter and presentation even if they changed after the last history entry. The returned view identifies where the host should select and scroll; navigation itself does not select a row or start playback.

### Focus an open view

`focusView(viewId)` accepts only a live view already present in `openViews`.
Invalid, unknown, and unopened ids are rejected.
Focusing the active view succeeds without publication; a different open view produces one `Focus` commit and does not implicitly append history.

When an explicit reveal needs to navigate to a particular open view, `navigateToOpenView(viewId)` preserves that view's filter and presentation. Unlike focus, a change of view records the live departure and the exact destination together in one `Navigation` commit. Back restores the departure, including its quick filter and column sort. An already-active, available target is a no-op. A closed view or one whose saved List has already been deleted is rejected without changing history, even if the deletion notification has not yet closed the view.

There is no public raw add-view operation.
Opening and adopting a view is part of semantic navigation or session restoration.

### Change active presentation

`setActivePresentation(spec, presentationChangeOptions)` requires an active live view, normalizes the complete specification, applies it through `ViewService`, and optionally commits the resulting navigation point.
The full-spec setter on `ViewService` is private; `WorkspaceService` uses it for active presentation changes, navigation overrides, and history replay. New views receive their full spec during creation, including during session restore.
Presentation-change options contain only the history-recording policy and cannot carry navigation target or presentation-mode fields.
The complete presentation value participates in equality, including visible and redundant fields.

`setActivePresentation(presetId, presentationChangeOptions)` takes a typed `TrackPresetId` and resolves either the `BuiltinPreset` or the workspace `CustomPresetId` directly. It returns the resolved presentation.
A custom id absent from the workspace returns `NotFound`; a missing active view for a resolved preset returns `InvalidState`. Text consumers reject malformed keys before calling this command.
The resolved definition has no header override, so choosing the active preset again clears one.
Reapplying an identical presentation and history point succeeds without publication.

### Sort a view by column

`ViewService::setColumnSort(viewId, sort)` is the public command for a column-header sort. It takes a view id and one `TrackColumnSort`, the clicked `TrackField` and its direction. It stores that value in `optColumnSort` and leaves `presetId`, base `sortBy`, grouping, and the field lists unchanged. The override is not a preset and has no catalog caption.

An unknown view returns `NotFound`. A column with no sort mapping returns `InvalidInput`. An unchanged presentation succeeds without publishing. An effective change publishes `ViewService::PresentationChanged`, not `WorkspaceChanged`, and does not change the workspace revision, history cursor, or the List's saved presentation preference.

The applied row keys are [`effectiveSortBy()`](../presentation/track-presentation.md#invariants), a sequence of `TrackSortTerm` values. The clicked column's sort key is first and the preset's remaining keys break ties. Click history is not a sort key. `DisplayTrackNumber` and `TrackNumber` both project to `TrackSortField::TrackNumber`, so either column sorts disc number before track number, in the same direction. `Movement` and `MovementNumber` both project to `TrackSortField::Movement`. The header arrow follows the clicked column, not that shared sort key.

A grouped table and playback do not apply that sequence as one order. [Track-list presentation](../presentation/track-presentation.md#grouping-and-sorting) owns the split: section order stays on the preset's base sort, within-section rows use the effective sequence, and playback launch copies only the flat effective sequence into a detached projection with no groups.

The header arrow and the next click target `TrackColumnSort::column`, not the shared sort key and not the inserted disc-number term. `nextColumnSort(column)` is ascending unless the current override is already that same column and ascending. Clicking the other column of a shared-key pair therefore starts ascending. Choosing the already selected preset applies its definition and clears the override. With the default history-recording policy, that choice first checkpoints the override if the departure matches the current point as described below, then records the cleared preset as the new point.

A recorded departure can update only the current history point's column override. It does so when the live view has an override and the List, quick filter, and entire presentation apart from `optColumnSort` match the point. Sharing a preset id is not enough: grouping, base sort, and field lists must also match. The update does not add a stop or discard Forward entries; a new destination still follows the ordinary history commit rules. Repeating navigation to the already-active view after a column sort uses that checkpoint and does not add a stop or discard Forward. The request may omit a presentation, carry `NewViewDefault`, or carry an equivalent override. `NewViewDefault` is ignored for the reused view, and the repeat is compared after the checkpoint, not against the point from before the sort. [Session capture](session.md) independently saves the exact live presentation, including the override.

### Traverse history

`goBack()` and `goForward()` copy history, refresh a matching departure point's header override as described above, then move only the candidate cursor. A different quick filter does not become a new stop during traversal.
Replay reuses a live view already open in the workspace that matches list and filter or creates a replacement. It restores the exact presentation, prepares the next open/focused snapshot, and commits the snapshot and history candidate once.
Replay never appends a history point or discards Forward entries.

`canGoBack()` and `canGoForward()` expose the current cursor boundaries without duplicating history state.
They are owner-thread-confined reads and publish no separate availability signal; a frontend rereads them after the existing workspace-changed observation.

Traversal past an end returns `NotFound`.
Failed target recreation or presentation restoration leaves the live cursor, snapshot, and revision unchanged.

### Close views and delete lists

`closeView(viewId)` removes an open view, selects the last remaining open view as fallback when necessary, commits once, and releases the view through `ViewService`.
Closing an id not present in the workspace succeeds without publication and does not call view destruction.

A committed library-list deletion closes every matching open view in one `ListDeletion` commit, even when several filtered views use the same base list.
A complete library reset, such as a YAML restore, names no deleted list, so the workspace instead closes, in the same kind of commit, every open view whose base list the library no longer has; All Tracks and other virtual lists stay open.
History points remain semantic snapshots; replaying a point for a deleted list fails during reconstruction and leaves the live cursor unchanged.
When the history cursor still names a saved List that no longer exists, a later successful recorded navigation commits the live fallback view as the departure in that same candidate. A failed destination does not record it. Back from the new stop returns to the recovered view and does not try to recreate the deleted List.

### Edit custom presets

A custom preset must carry a `CustomPresetId` in `presetId`. A built-in id returns `InvalidInput`. `optColumnSort` also returns `InvalidInput`: a preset definition cannot contain a view-local header override.
Copying the live view into that definition keeps the grouped table's section and within-section order, not the detached flat playback order. [Track-list presentation](../presentation/track-presentation.md#grouping-and-sorting) owns when a grouping-column override is left out of the new base sort.
Adding a new preset or changing a preset with the same custom id produces one `Presets` commit.
The label is display text, not replacement identity.
Adding an identical value or removing an absent id succeeds without publication.
Removing an existing preset likewise commits one complete preset collection.
The [preset reference](../../reference/presentation/track-preset.md) and [workspace session-state reference](../../reference/workspace/session-state.md) own stable preset identities and their serialized form.

### Reveal an album

Album reveal is not a `WorkspaceService` command.
`AppRuntime::jumpToAlbum(trackId)` rejects the invalid track id, navigates to All Tracks with an explicit `Override` carrying the built-in albums presentation, and submits playback reveal for the returned active view.
Success acknowledges submission, not a host having scrolled the row into view. The navigation is already committed when the host receives the reveal request.

## Preparation and commit

Commands copy the small workspace snapshot and bounded history before installing either value.
View creation and presentation updates complete before the no-fail aggregate swap.
`ViewService::createView()` itself publishes no creation signal, so a synchronously prepared view is not exposed as a partial workspace aggregate.

Session restore creates every view first and destroys all created candidates if a later creation fails.
Navigation and history replay reuse only views from the committed open set, so a destruction observer cannot adopt another candidate still being rolled back.
Successful restore installs all ids, focus, presets, and one revision together.
The implementation deliberately does not add a generic transaction object, detached candidate lease, command generation, or asynchronous command state machine.

## Executor, observation, and lifetime

Workspace reads, subscriptions, and commands are confined to the runtime callback executor.
An off-executor call is a programming error and fails fast rather than racing mutable state.
Commands are bounded and synchronous and expose no cancellation point.

`snapshot()` returns the current value by copy.
`onChanged()` delivers `WorkspaceChanged{snapshot, cause}` on a later callback-executor turn.
The event owns its snapshot, so a command called by an observer cannot mutate the event currently being delivered; its own observation is queued separately.

Observers are ordinary callables behind the owning signal boundary (see [signal delivery](../execution/signal.md)): the snapshot they receive is already committed, so an observer cannot change an already-returned command result, and an escaping exception enters AO fatal handling rather than leaving a diverged replica rendering.
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

- [`NavigationHistoryTest.cpp`](../../../test/runtime/NavigationHistoryTest.cpp) proves cursor and capacity mechanics.
- [`WorkspaceNavigationTest.cpp`](../../../test/runtime/WorkspaceNavigationTest.cpp) proves validation, command results, complete observations, reentrancy, observer delivery, deletion and reset batching, source-first navigation, and album reveal.
- [`WorkspaceHistoryTest.cpp`](../../../test/runtime/WorkspaceHistoryTest.cpp) proves atomic history replay and boundary availability.
- [`WorkspacePresentationTest.cpp`](../../../test/runtime/WorkspacePresentationTest.cpp) proves presentation and preset commands.
- [`WorkspaceSessionTest.cpp`](../../../test/runtime/WorkspaceSessionTest.cpp) proves atomic multi-view restore and failure cleanup.
- [`HeadlessShellTest.cpp`](../../../test/runtime/HeadlessShellTest.cpp) proves frontend-neutral reconstruction.

## Related documents

- [Workspace architecture](README.md)
- [Runtime execution architecture](../execution/README.md)
- [Workspace session](session.md)
- [Workspace session state](../../reference/workspace/session-state.md)
- [Track filtering](../presentation/track-filter.md)
- [Track-list presentation](../presentation/track-presentation.md)
