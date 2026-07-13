---
id: presentation.selection-summary
type: spec
status: current
domain: presentation
summary: Defines selected-track count and duration aggregation and their platform-neutral status text.
---
# Selection-summary specification

## Scope

This specification owns the semantic summary of the active track-list selection: selected count, aggregate known duration, and platform-neutral display text.
It does not own selection commands, layout placement, runtime activity reporting, or widget rendering.

Selection lifecycle belongs to the [workspace architecture](../../architecture/workspace.md).
Independent operational activity belongs to the [activity-status specification](activity-status.md).

## Code boundary

Runtime `ViewService` owns the selected track ids and calculates aggregate duration against one library read transaction.
The pure formatter lives in `ao::uimodel` under `app/include/ao/uimodel/library/track/` and `app/uimodel/library/track/`.
GTK `SelectionInfoLabel` subscribes to runtime selection changes and renders the UIModel text.

The formatter cannot read the library, subscribe to runtime, or depend on a frontend toolkit.
The runtime aggregation cannot choose human-readable wording.

## Terminology

- **Selection count** is the number of ids stored in the runtime view selection.
- **Selection duration** is the sum of known track durations for those ids.
- A **stale id** is selected in the view but absent from the current library snapshot.

## Invariants

- An empty selection produces empty summary text.
- Singular and plural count text are distinct.
- The duration suffix is present only when the supplied total is greater than zero.
- Duration formatting uses the shared track-field duration formatter.
- Aggregation uses one read transaction, visits each selected id once, and skips stale ids.
- An unknown view id and an empty selection both aggregate to zero.
- Selection summary and activity status are independent presentation responsibilities and may be placed separately by a shell layout.

## State model

The runtime view owns the authoritative vector of selected track ids and emits `SelectionChanged` observations.
The summary is derived state; it is not persisted and has no separate identity or revision.

The UIModel formatter accepts a count and an optional duration.
It does not retain either value after returning the string.

## Commands and transitions

On a selection change, a frontend reads the current selection count and requests `ViewService::selectionDuration(viewId)`.
It passes the derived values to `trackSelectionSummaryText` and replaces the visible label.

The text contract is:

| Input | Result |
| --- | --- |
| zero selected | empty string |
| one selected, no positive duration | `1 item selected` |
| multiple selected, no positive duration | `N items selected` |
| positive aggregate duration | count text followed by ` (duration)` |

The shared duration formatter uses `m:ss` below one hour and `h:mm:ss` at or above one hour.

## Failure and cancellation

Formatting is synchronous and cannot fail.
Runtime storage read failures are narrowed by the shared storage-observation policy used by `ViewService`; an unreadable or missing selected record contributes no duration.
The aggregation exposes no retry or cancellation surface because it is a bounded synchronous view query.

## Persistence and versioning

The derived summary is never persisted.
Selection persistence, when present as part of a workspace snapshot, is owned by the [workspace session specification](../workspace/session.md); restored ids are still resolved against current library state before they contribute duration.

## Frontend observations

GTK layouts place `status.selectionInfo` independently from `status.activityStatus` and `status.trackCount`.
`SelectionInfoLabel` is global to the active runtime view rather than coupled to a particular GTK selection model.

TUI may render the same semantic values in terminal-specific geometry.
Frontend placement, separators, truncation, and accessibility labels do not change the formatter contract.

## Implementation map

- [`ViewService.cpp`](../../../app/runtime/ViewService.cpp) owns selection-duration aggregation.
- [`TrackSelectionSummary.cpp`](../../../app/uimodel/library/track/TrackSelectionSummary.cpp) owns count and duration text.
- [`TrackFieldFormatter.cpp`](../../../app/uimodel/field/TrackFieldFormatter.cpp) owns duration formatting.
- [`SelectionInfoLabel.cpp`](../../../app/linux-gtk/track/SelectionInfoLabel.cpp) adapts runtime observation to GTK.

## Test map

- [`TrackSelectionSummaryTest.cpp`](../../../test/unit/uimodel/library/track/TrackSelectionSummaryTest.cpp) protects the pure text contract.
- [`ViewServiceSelectionTest.cpp`](../../../test/unit/runtime/ViewServiceSelectionTest.cpp) protects aggregation, stale-id skipping, empty selection, and unknown views.
- [`SelectionInfoLabelTest.cpp`](../../../test/unit/linux-gtk/track/SelectionInfoLabelTest.cpp) protects GTK binding behavior.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Workspace architecture](../../architecture/workspace.md)
- [Workspace navigation specification](../workspace/navigation.md)
- [Activity-status specification](activity-status.md)
