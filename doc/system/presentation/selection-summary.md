---
id: presentation.selection-summary
---
# Selection summary

The summary presents the active track-list selection's count and aggregate known duration.
It is independent of [activity status](activity-status.md), selection commands, and shell placement.

## Aggregation and presentation contract

Runtime `ViewService` owns the selected ids.
The count is the number of ids in that selection; duration aggregation uses one library read transaction, visits each selected id once, and skips records absent from that snapshot.
An unknown view and an empty selection both aggregate to zero.
A missing record contributes no duration; failure to open the transaction follows the library's exceptional read contract.

The UIModel formatter takes the count, optional duration, and injected text catalog without reading the library or retaining state.
An empty selection produces empty text.
A nonempty selection uses the catalog's count grammar and includes a duration only when the supplied total is positive.
The shared duration formatter uses `m:ss` below one hour and `h:mm:ss` at or above one hour.
Exact translated wording belongs to the [catalog assets](../../../app/i18n/catalog/root.txt), not a second table here.

On `SelectionChanged`, GTK's `SelectionInfoLabel` reads the active runtime view's count and duration and replaces its label through the UIModel formatter.
It is not bound to a particular GTK selection model.
Other frontends may place or truncate the same semantic values differently without changing aggregation.

The summary is synchronous derived state with no independent persistence, identity, revision, retry, or cancellation operation.
[Workspace restoration](../workspace/session.md) reconstructs views, not selection: the session does not persist selected ids.
Any live selection supplied after restoration is aggregated against the current library snapshot under the same rules above.

## Implementation and tests

- [`ViewService.cpp`](../../../app/runtime/ViewService.cpp) and [`ViewServiceSelectionTest.cpp`](../../../test/unit/runtime/ViewServiceSelectionTest.cpp): one-snapshot aggregation, stale ids, empty selection, and unknown views.
- [`TrackSelectionSummary.cpp`](../../../app/uimodel/library/track/TrackSelectionSummary.cpp), [`TrackFieldFormatter.cpp`](../../../app/uimodel/field/TrackFieldFormatter.cpp), and [`TrackSelectionSummaryTest.cpp`](../../../test/unit/uimodel/library/track/TrackSelectionSummaryTest.cpp): count and duration presentation.
- [`SelectionInfoLabel.cpp`](../../../app/linux-gtk/track/SelectionInfoLabel.cpp) and [`SelectionInfoLabelTest.cpp`](../../../test/unit/linux-gtk/track/SelectionInfoLabelTest.cpp): GTK binding and active-view behavior.

See [workspace](../workspace/README.md) for selection ownership and [presentation](README.md) for the runtime/UIModel/frontend boundary.
