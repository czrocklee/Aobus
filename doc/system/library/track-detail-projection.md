---
id: library.track-detail-projection
---
# Track-detail projection

## Scope

This specification defines how a live detail projection resolves a selection target and produces frontend-neutral track-field, Credits, cover, tag, and custom-metadata snapshots.
It does not own field definitions, which belong to the [track model reference](../../reference/library/model/track.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../overview.md).
Its public boundary is `app/include/ao/rt/projection/`, its implementation is `app/runtime/projection/`, and it may consume ViewService, WorkspaceService, runtime change values, and core library reads without depending on UIModel or frontends.

## Target model

- `FocusedViewTarget` follows the workspace's active view and that view's current selection.
- `ExplicitViewTarget` follows the selection of one fixed live `ViewId`.
- `ExplicitSelectionTarget` snapshots the supplied track ids and does not follow view selection.

An explicit target whose view is already absent starts with an empty snapshot.
When its live view is destroyed, the projection clears and publishes its snapshot and stops tracking that runtime id.

No selected ids produce `SelectionKind::None`, one produces `Single`, and more than one produces `Multiple`.
The snapshot retains requested ids even when some or all no longer resolve to stored tracks.

## Aggregation

One read transaction supplies all stored fields for a rebuilt snapshot.
Synthetic fields and the tag-list field are not part of generic field aggregation.

### Which revision does a snapshot represent?

`TrackDetailSnapshot::libraryRevision` is the committed revision of the read transaction that built the snapshot, not a claim that the snapshot follows every later library commit.
A rebuild with an empty selection still performs a library read and carries that read's revision.
By contrast, an explicit view target that cannot be resolved during initial construction retains the default empty snapshot at revision zero; that zero means no current library read was captured.
Unrelated commits need not rebuild the projection, so consumers must use this revision as evidence for the captured data rather than as the latest global revision.

For each non-synthetic field, identical loaded values produce one `optValue`; differing values produce `mixed = true` with no value.
If no requested track resolves, aggregate fields remain empty.

Credits aggregate independently in four owning kind sections.
Each section compares complete ordered entries, including names, roles, and duplicate multiplicity; identical loaded sections produce one common value, and differing sections are mixed with no representative list.
A loaded empty section is a present common empty value, not a missing track.
Missing requested tracks contribute no values: surviving tracks still supply common or mixed sections, while a selection with no loaded track leaves every section absent.
Selection kind and requested ids remain unchanged by these display omissions.
This display contract does not authorize surviving-subset edits; [editor binding](../presentation/metadata-editing.md#preparing-a-coherent-properties-baseline) requires every requested target to exist.

Custom metadata is sorted by key.
Each item reports whether it appears on any or all loaded tracks, and whether its values differ.

A single-track snapshot exposes that track's primary cover resource and tag ids.
Current multi-selection snapshots do not compute a tag intersection or a shared cover value.

## Refresh and publication

Construction builds the initial snapshot.
Subscription immediately receives the current snapshot.

Focus or tracked-view selection changes rebuild and publish a snapshot.
A tracked-view destruction clears and publishes the snapshot.
A library reset or inserted, deleted, or mutated track intersecting the retained selection also rebuilds and publishes.
Unrelated track changes do not publish.

Each publication occurs after final snapshot state is installed.

## Failure and lifetime

Missing tracks and missing explicit views contribute no loaded values and are not fatal.
Storage and invariant failures are not translated into projection availability; an exception escaping callback
delivery is fatal at the owning signal boundary.

The projection releases workspace, view, and library-change subscriptions before those borrowed owners are destroyed.
Its observer signal weak-invalidates outstanding subscriptions when the projection itself is destroyed.

## Implementation map

- [`TrackDetailProjection.h`](../../../app/include/ao/rt/projection/TrackDetailProjection.h) defines the concrete projection, targets, aggregate values, and snapshots.
- [`TrackDetailProjection.cpp`](../../../app/runtime/projection/TrackDetailProjection.cpp) owns target tracking, aggregation, and refresh behavior.

## Test map

- [`TrackDetailProjectionTest.cpp`](../../../test/unit/runtime/projection/TrackDetailProjectionTest.cpp) proves target following, immediate subscription, tracked-view destruction, intersecting refresh, common/mixed fields, missing tracks and views, single-track tags, custom metadata aggregation, and the distinction between read-backed empty snapshots and unresolved revision-zero snapshots.
- [`TrackDetailPerformanceTest.cpp`](../../../test/unit/runtime/projection/TrackDetailPerformanceTest.cpp) proves owning per-kind Credits, common empty sections, complete ordered equality, mixed sections, independent common categories, and intersecting refresh.

## Related documents

- [Library architecture](structure.md)
- [Track model reference](../../reference/library/model/track.md)
- [Library change publication](change-publication.md)
