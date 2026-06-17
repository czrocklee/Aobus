# Selection Status Summary

The status bar (`StatusSlot`) shows a summary of the current track selection when
no library task or notification is active. It is rendered by `SelectionInfoLabel`
(`app/linux-gtk/track/SelectionInfoLabel`), which subscribes to
`rt::ViewService::onSelectionChanged`.

## Displayed text

The text comes from the pure formatter
`ao::uimodel::track::selectionSummaryText(count, totalDuration)`
(`app/include/ao/uimodel/track/SelectionSummary.h`):

| Selection            | Text                          |
|----------------------|-------------------------------|
| nothing selected     | *(empty)*                     |
| one track            | `1 item selected`             |
| many tracks          | `N items selected`            |
| with total duration  | `N items selected (h:mm:ss)`  |

The duration suffix is appended only when the total is positive, and is formatted
by `ao::uimodel::track::formatDuration` (so `< 1h` renders as `m:ss`).

## Where the duration comes from

`SelectionInfoLabel` is wired to the runtime `ViewService` (it is part of the
global status bar and is intentionally decoupled from any single track page's GTK
selection model). The total duration is therefore computed runtime-side:

- `rt::ViewService::selectionDuration(viewId)` reads the view's stored selection,
  opens a library read transaction, and sums `TrackView.property().duration()`
  for each selected id (ids missing from the library are skipped).

This is O(selection size) per selection change — one read transaction, consistent
with the existing GTK-side `TrackSelectionController::selectedTracksDuration()`.

## Testing

- Text contract: `test/unit/uimodel/track/SelectionSummaryTest.cpp` (pure).
- Duration aggregation: `test/unit/runtime/ViewServiceTest.cpp`
  (`selectionDuration` sums seeded track durations, skips missing ids, returns 0
  for empty/unknown views).
- Widget smoke: `test/unit/linux-gtk/track/SelectionInfoLabelTest.cpp` covers the
  count text; duration correctness is proven at the layers above rather than
  re-proven through the widget.
