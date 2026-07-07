# Selection Status Summary

## Layout placement

The Modern layout renders selection status in the top-right header through the
`status.selectionInfo` layout component, next to `status.trackCount`. The Modern
bottom-right bar uses `status.activityStatus` as an ambient runtime readout for
library task progress, transient completion text, and persistent warning/error
state. It is hidden while idle.

The default layout composes the same two responsibilities explicitly:
`status.activityStatus` keeps the old status-slot position at the left edge of
the right-side status group, and `status.selectionInfo` follows it before the
track-count separator. The activity component uses the `classicInline` variant
without taking idle space, so the idle classic bar still reads as selection info
plus track count without a blank activity slot. The old combined
`status.statusSlot` component is intentionally removed so selection state and
runtime activity state can be positioned independently by layout presets.

`ActivityStatusViewModel` adapts runtime notifications and active library task
progress into the shared compact/detail view state. Its private
`ActivityStatusFeedState` keeps the detail/feed state for unresolved runtime
notifications and task progress. The GTK `status.activityStatus` component renders
the compact inline readout and opens a minimal detail popover only when detail
content exists and the compact readout is visible. The popover renders active task
detail and a bounded set of qualifying notification rows; it intentionally has no
empty state, unread badge, or full feed stack.
Clearable notification detail rows have a local dismiss affordance. This hides the
row from `ActivityStatus` and removes that source from the compact activity
projection without calling `rt::NotificationService::dismiss`, so the runtime feed
entry remains available to other consumers.
Compact dismiss is also a local activity-status suppression. In hidden-idle
layouts, dismissing compact status can remove the visible affordance for that
notification until new qualifying activity appears or another consumer opens the
runtime feed.

Notification detail actions are rendered only when `ActivityStatus` is constructed
with an explicit action resolver and handler. The layout component validates each
notification action against the layout `ActionRegistry`: unknown actions are not
shown, disabled actions are shown insensitive with the registry disabled reason,
and empty notification labels fall back to the registered action label. Activation
uses the concrete action button as the anchor, so a notification action id must
name a registered layout action before it can render or execute. Rows render only
a small bounded action set and do not infer behavior from the action id inside the
widget. Action execution never implies feed dismissal; actions that resolve a
notification must update or dismiss the runtime notification explicitly.

Theme CSS keeps the two placements visually distinct. Modern treats activity as a
low-contrast ambient readout with soft popover rows and only subtle warning/error
fills. Classic keeps the same data dense and status-bar-like: square edges, small
controls, compact detail rows, and hard progress geometry.

Runtime notifications opt into the activity projection through
`rt::NotificationActivityPresentation`:

- `Default` can create compact status and detail rows according to model policy.
- `DetailOnly` is included in `ActivityDetailState` without creating compact
  status. In layouts where `ActivityStatus` is hidden while idle, these entries
  are not user-reachable through the compact readout alone.
- `Hidden` stays in `rt::NotificationService` but is excluded from activity
  compact status, detail rows, and locally hideable activity ids. The startup
  `Aobus Ready` notification uses this mode.

Detail rows are intentionally narrower than the runtime notification feed. Plain
`Default` info notifications may appear as transient compact text, but they do
not make the activity detail surface openable. Detail is openable when
`hasDetailContent(detail)` is true and the compact readout is visible: active
library task detail is present, or at least one notification qualifies as detail
content. Qualifying notifications are warning/error severities, sticky
notifications, progress notifications, rich notifications with title/icon/actions,
or any notification explicitly marked `DetailOnly`.

Both paths reuse `SelectionInfoLabel` (`app/linux-gtk/track/SelectionInfoLabel`),
which subscribes to `rt::ViewService::onSelectionChanged`.

## Displayed text

The text comes from the pure formatter
`ao::uimodel::selectionSummaryText(count, totalDuration)`
(`app/include/ao/uimodel/library/track/TrackSelectionSummary.h`):

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
- Runtime activity status: `test/unit/uimodel/status/activity/ActivityStatusFeedState*Test.cpp`
  covers progress priority, transient completion, warning/error persistence, and
  compact dismissal semantics.
