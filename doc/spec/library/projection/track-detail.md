---
id: library.track-detail-projection
type: spec
status: current
domain: library
summary: Defines live track-detail target resolution, multi-selection aggregation, refresh, and snapshot publication.
---
# Track-detail projection

## Scope

This specification defines how a live detail projection resolves a selection target and produces frontend-neutral track-field, cover, tag, and custom-metadata snapshots.
It does not own field definitions, which belong to the [track model reference](../../../reference/library/model/track.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
Its public boundary is `app/include/ao/rt/projection/`, its implementation is `app/runtime/projection/`, and it may consume ViewService, WorkspaceService, runtime change values, and core library reads without depending on UIModel or frontends.

## Target model

- `FocusedViewTarget` follows the workspace's active view and that view's current selection.
- `ExplicitViewTarget` follows the selection of one fixed `ViewId`.
- `ExplicitSelectionTarget` snapshots the supplied track ids and does not follow view selection.

No selected ids produce `SelectionKind::None`, one produces `Single`, and more than one produces `Multiple`.
The snapshot retains requested ids even when some or all no longer resolve to stored tracks.

## Aggregation

One read transaction supplies all stored fields for a rebuilt snapshot.
Synthetic fields and the tag-list field are not part of generic field aggregation.

For each non-synthetic field, identical loaded values produce one `optValue`; differing values produce `mixed = true` with no value.
If no requested track resolves, aggregate fields remain empty.

Custom metadata is sorted by key.
Each item reports whether it appears on any or all loaded tracks, and whether its values differ.

A single-track snapshot exposes that track's primary cover resource and tag ids.
Current multi-selection snapshots do not compute a tag intersection or a shared cover value.

## Refresh and publication

Construction builds the initial snapshot without incrementing its publication revision.
Subscription immediately receives the current snapshot.

Focus or tracked-view selection changes rebuild and publish a snapshot.
A library reset or inserted, deleted, or mutated track intersecting the retained selection also rebuilds and publishes.
Unrelated track changes do not advance the detail revision.

Each publication increments the projection-local revision after final snapshot state is installed.

## Failure and lifetime

Missing tracks contribute no loaded values and are not fatal.
Unexpected storage failures follow the runtime storage-result boundary.

The projection releases workspace, view, and library-change subscriptions before those borrowed owners are destroyed.

## Implementation map

- [`TrackDetailProjection.h`](../../../../app/include/ao/rt/projection/TrackDetailProjection.h) defines targets, aggregate values, and snapshots.
- [`LiveTrackDetailProjection.h`](../../../../app/include/ao/rt/projection/LiveTrackDetailProjection.h) defines the live projection boundary.
- [`LiveTrackDetailProjection.cpp`](../../../../app/runtime/projection/LiveTrackDetailProjection.cpp) owns target tracking, aggregation, and refresh behavior.

## Test map

- [`TrackDetailProjectionTest.cpp`](../../../../test/unit/runtime/projection/TrackDetailProjectionTest.cpp) proves target following, immediate subscription, intersecting refresh, common/mixed fields, missing tracks, single-track tags, and custom metadata aggregation.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Track model reference](../../../reference/library/model/track.md)
- [Library change publication](../runtime/change-publication.md)
