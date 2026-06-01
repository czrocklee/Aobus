# Navigation History Plan

This document is the entry point for the navigation history implementation
series. The design replaces ad-hoc pre-mutation recording with committed
workspace navigation snapshots.

## Goals

- Provide browser-like back and forward navigation for list switches,
  ad-hoc query jumps, presentation changes, and album jumps.
- Keep history ownership in `WorkspaceService`.
- Keep `ViewService` as the low-level view state service.
- Store enough state to restore the exact view shape that the user saw,
  including custom presentation details.
- Remove the legacy group-only presentation API after the history model no
  longer depends on it.

## Non-Goals

- Persist navigation history across app restarts.
- Record quick-filter typing.
- Record sidebar/panel toggles.
- Record scroll-only actions such as reveal-current-track.
- Store destroyed view ids in history. History stores restoreable view
  snapshots, not object identity.

## Document Series

1. [Model and API](navigation-history-01-model-api.md)
2. [Implementation Plan](navigation-history-02-implementation.md)
3. [`setGrouping` Cleanup Plan](navigation-history-03-setgrouping-cleanup.md)
4. [Test Cases and Data](navigation-history-04-test-cases.md)

## Key Design Decision

History records the resulting committed state after a workspace navigation
command completes.

Example:

```text
Initial current view: A
navigate to B        -> commit B
navigate to C        -> commit C
back                 -> restore B
forward              -> restore C
```

This is intentionally different from a public `recordCurrentState()` API that
callers must invoke before mutating state. The pre-mutation API is fragile:
every UI caller must remember the ordering rule, and multi-step commands such
as "jump to album" either double-record or lose part of the final state.

## Implementation Order

1. Add pure `NavigationHistory`.
2. Add workspace-level navigation commands and committed snapshot recording.
3. Route GTK navigation entry points through `WorkspaceService`.
4. Add back/forward mouse handling.
5. Migrate presentation state to `TrackPresentationSpec`.
6. Remove `setGrouping` and `onGroupingChanged`.
7. Run unit and GTK integration tests listed in the test plan.

## Files

Expected final file set:

| Action | File | Purpose |
|---|---|---|
| New | `app/include/ao/rt/NavigationHistory.h` | Pure history data structure |
| New | `app/rt/NavigationHistory.cpp` | History implementation |
| Modify | `app/include/ao/rt/WorkspaceService.h` | Workspace navigation API |
| Modify | `app/rt/WorkspaceService.cpp` | Snapshot commit, restore, commands |
| Modify | `app/include/ao/rt/StateTypes.h` | Remove stale `navigationStack`, migrate config presentation |
| Modify | `app/include/ao/rt/ViewService.h` | Remove group-only API |
| Modify | `app/rt/ViewService.cpp` | Presentation-only state mutation |
| Modify | `app/linux-gtk/track/TrackPresentationButton.cpp` | Use workspace presentation command |
| Modify | `app/linux-gtk/layout/components/PlaybackComponents.cpp` | Use workspace album-jump command |
| Modify | `app/linux-gtk/app/MainWindow.cpp` | Mouse buttons 8/9 |
| Modify | `app/CMakeLists.txt` | Add `NavigationHistory.cpp` |
| Modify | `test/CMakeLists.txt` | Add new runtime tests if needed |

