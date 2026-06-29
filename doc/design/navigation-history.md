# Navigation History

Aobus stores workspace navigation history as committed view snapshots owned by
`WorkspaceService`.

## Snapshot

Each history point stores the view state needed to restore what the user saw:

- `listId`
- `filterExpression`
- `TrackPresentationSpec`

History deliberately does not store runtime object identity. View ids can be
closed or recreated, so restoring a history point resolves or creates the target
view from the snapshot.

History also does not store selection, scroll position, sidebar state, or other
transient editing/layout state.

## Commit Model

Navigation commands commit the resulting active view after the command
completes.

```text
start on A
navigate to B   -> commit B
navigate to C   -> commit C
back            -> restore B
forward         -> restore C
```

This keeps multi-step commands as one history event. For example, jumping to an
album can focus a view, apply the album presentation, reveal the track, and then
commit the final state once.

`NavigationOptions::recordHistory = false` suppresses recording for internal
steps or explicit caller choices. During replay, `WorkspaceService` suppresses
new commits so `goBack()` and `goForward()` do not grow history.

## Traversal

`NavigationHistory` provides browser-like traversal:

- committing the current point is deduplicated
- committing after going back truncates forward history
- capacity eviction removes the oldest points
- `canGoBack` / `canGoForward` reflect the current history index

`WorkspaceService` emits `NavigationHistoryChanged` when the back/forward
availability changes or after replay restores a point.

## Boundaries

History is a runtime navigation aid, not persisted session state. Session
restore may recreate the active view, but it does not restore the full
back/forward stack.

Quick-filter typing is not a navigation command by itself. Commands that create
or restore filtered views must still use an explicit list base plus the filter
expression in the snapshot.
