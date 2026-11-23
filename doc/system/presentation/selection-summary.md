---
id: presentation.selection-summary
---

# Selection summary

The summary presents the active track-list selection's count and aggregate known duration.
It is independent of [activity status](activity-status.md), selection commands, and shell placement.

## Aggregation and presentation contract

Runtime `ViewService` owns the selected ids.
The count is the number of stored selection ids, not the number of currently visible or realized rows. Duration aggregation uses one library read transaction and walks that same selection, skipping records absent from the snapshot.
An unknown view and an empty selection both aggregate to zero.
A missing record contributes no duration; failure to open the transaction follows the library's exceptional read contract.

The UIModel formatter takes the count, optional duration, and injected text catalog without reading the library or retaining state.
An empty selection produces empty text.
A nonempty selection uses the catalog's count grammar and includes a duration only when the supplied total is positive.
The shared duration formatter uses `m:ss` below one hour and `h:mm:ss` at or above one hour.
Exact translated wording belongs to the [catalog assets](../../../app/i18n/catalog/root.txt), not a second table here.

GTK's `SelectionInfoLabel` and WinUI's `status.selectionInfo` read the runtime selection on binding and when the active view changes, even without a new row gesture. They use the shared formatter for both count and duration. Changes in an inactive view's selection do not update the summary until that view becomes active. In WinUI a retired shell generation no longer updates its summary, and the summary can be hidden by shell layout without changing the selection.

A settled library change can change duration without changing the view or its selected ids. Both frontends recompute a nonempty selection's duration after a library reset or a committed insertion, mutation, or deletion touching one of those ids. For example, a rescan can replace a selected track's stored duration; a deleted track still in the selection contributes to the count but not the duration. Unrelated library changes and viewport updates do not trigger aggregation.

The summary is not an admission check for playback: [primary Play](../playback/README.md#primary-play) needs a visible selected track when starting a new desktop selection.

The summary is synchronous derived state with no independent persistence, identity, revision, retry, or cancellation operation.
[Workspace restoration](../workspace/session.md) reconstructs views, not selection: the session does not persist selected ids.
Any live selection supplied after restoration is aggregated against the current library snapshot under the same rules above.

## Implementation and tests

- [`ViewService.cpp`](../../../app/runtime/ViewService.cpp) and [`ViewServiceSelectionTest.cpp`](../../../test/runtime/ViewServiceSelectionTest.cpp): one-snapshot aggregation, stale ids, empty selection, and unknown views.
- [`TrackSelectionSummary.cpp`](../../../app/uimodel/library/track/TrackSelectionSummary.cpp), [`TrackFieldFormatter.cpp`](../../../app/uimodel/field/TrackFieldFormatter.cpp), and [`TrackSelectionSummaryTest.cpp`](../../../test/uimodel/library/track/TrackSelectionSummaryTest.cpp): count and duration presentation and committed-change filtering.
- [`SelectionInfoLabel.cpp`](../../../app/linux-gtk/track/SelectionInfoLabel.cpp) and [`SelectionInfoLabelTest.cpp`](../../../test/linux-gtk/track/SelectionInfoLabelTest.cpp): GTK binding, active-view changes, and same-selection rescan and deletion.
- [`StatusComponents.cpp`](../../../app/windows-winui/layout/component/status/StatusComponents.cpp) owns WinUI binding and generation admission. The [native automation probe](../../../test/winui/probe/WinUiAutomationProbe.cpp) checks the live summary across selection, view changes, a committed rescan, and shell replacement.

See [workspace](../workspace/README.md) for selection ownership and [presentation](README.md) for the runtime/UIModel/frontend boundary.
