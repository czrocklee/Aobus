---
id: library.track-list-projection
type: spec
status: current
domain: library
summary: Defines live track-list projection ordering, grouping, incremental deltas, invalidation, and arena rebasing.
---
# Track-list projection

## Scope

This specification defines how a live track-list projection converts one leased ordered source into frontend-neutral rows, group sections, lookup indexes, and projection deltas.
Track source membership belongs to the [track source specification](../source/track-source.md), while presentation sort/group policy belongs to [track-list presentation](../../presentation/track-presentation.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
Its public boundary is `app/include/ao/rt/projection/`, its implementation is `app/runtime/projection/`, and it consumes source leases plus runtime library reads without depending on ViewService, UIModel, or frontends.

## Terminology

- **Source snapshot** is the projection's ordered `TrackId` mirror of its source.
- **Projection order** is source order when no sort terms exist, otherwise stable presentation sort order.
- **Regular batch** contains sequential insert, remove, and update row ranges.
- **Reset** announces a complete replacement snapshot.
- **Source invalidation** is the terminal outcome of the leased source identity.

## Invariants

- A projection publishes only after its complete final rows, lookup index, and group sections are visible.
- Every regular batch transforms the subscriber's preceding row sequence into the already-installed final sequence.
- Row-range coordinates are interpreted after preceding ranges in the same batch.
- Reset and source invalidation are valid only as singleton batches.
- Empty presentation sort preserves source order exactly.
- Equal sort keys retain stable relative order.

## Incremental update

For a regular source batch, the projection first replays the source edits against its id snapshot and validates that result against the source's final order.

With no presentation sort, it rebuilds row order directly from the validated id snapshot.
Metadata-only source updates do not reread rows when no projected value depends on them.

With sorting, the projection retains untouched rows, rebuilds inserted or metadata-updated rows, stably sorts the touched subset, and merges it with the retained sorted subset.
The row lookup index and group spans are rebuilt once after the batch.

The shared `delta::RegularTrackEditScript` kernel derives, applies, and validates public projection ranges.
Malformed coordinates, duplicate identities, or a reducer mirror that diverges from final state are fail-fast programmer errors.

## Grouping and reset behavior

The active `TrackPresentationSpec` determines sort terms, grouping, visible fields, and redundant fields.
A projection never parses a filter expression; its source already owns the resulting membership before presentation shape is applied.
A section identity, order, or section-metadata change publishes `ProjectionReset`; membership-count-only changes may remain regular row deltas.

Changing presentation or receiving source reset performs a complete rebuild.
Source invalidation publishes one terminal invalidation batch and no later reset.

## Arena rebasing

Sort and group strings are immutable views in a `StringArena`.
Incremental updates schedule a full rebase when allocated arena bytes reach twice the post-rebuild baseline with a 64 KiB floor, or touched-row churn reaches 25 percent with a 256-row floor.
The rebase releases all old view holders before discarding the arena and rebuilding.

## Failure and lifetime

Library read failures follow the runtime storage-result boundary.
Internal delta and mirror violations use fail-fast contracts rather than a recovery reset.

Projection delivery is synchronous on the callback side.
A lease pins the source identity until the projection releases it, and the projection subscription releases before its source owner.

## Implementation map

- [`TrackListProjection.h`](../../../../app/include/ao/rt/projection/TrackListProjection.h) defines snapshots and public delta values.
- [`LiveTrackListProjection.h`](../../../../app/include/ao/rt/projection/LiveTrackListProjection.h) defines the live implementation boundary.
- [`LiveTrackListProjection.cpp`](../../../../app/runtime/projection/LiveTrackListProjection.cpp) owns incremental maintenance, grouping, and arena rebasing.
- [`TrackProjectionEditScript.cpp`](../../../../app/runtime/projection/TrackProjectionEditScript.cpp) adapts the shared edit algebra.

## Test map

Track-list projection tests under [`test/unit/runtime/projection/`](../../../../test/unit/runtime/projection/) prove lifecycle, sequential deltas, sorting, grouping, incremental equivalence, arena rebase, mutation behavior, and scale behavior.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Track sources](../source/track-source.md)
- [Track-list presentation](../../presentation/track-presentation.md)
- [Track preset reference](../../../reference/presentation/track-preset.md)
- [Runtime track field catalog](../../../reference/library/model/track-field.md)
