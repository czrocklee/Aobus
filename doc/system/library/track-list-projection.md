---
id: library.track-list-projection
---
# Track-list projection

## Scope

This specification defines how a live track-list projection converts one leased ordered source into frontend-neutral rows, group sections, lookup indexes, and projection deltas.
Track source membership belongs to the [track source specification](track-source.md), while presentation sort/group policy belongs to [track-list presentation](../presentation/track-presentation.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../overview.md).
Its public boundary is `app/include/ao/rt/projection/`, its implementation is `app/runtime/projection/`, and it consumes source leases plus runtime library reads without depending on ViewService, UIModel, or frontends.

## Terminology

- **Source snapshot** is the projection's ordered `TrackId` mirror of its source.
- **Projection order** is source order when no sort terms exist, otherwise stable presentation sort order.
- **Regular batch** contains sequential insert, remove, and update row ranges.
- **Reset** announces a complete replacement snapshot.
- **Source invalidation** is the terminal outcome of the leased source identity.
- **Text-ordering policy** is an optional runtime interface that derives one transient locale key without exposing ICU types. A projection may initially borrow the composition-owned policy supplied at construction; a live replacement passed to `setTextOrderingPolicy` is retained by shared ownership.

## Invariants

- A projection publishes only after its complete final rows, lookup index, and group sections are visible.
- Every regular batch transforms the subscriber's preceding row sequence into the already-installed final sequence.
- Row-range coordinates are interpreted after preceding ranges in the same batch.
- Reset and source invalidation are valid only as singleton batches.
- Empty presentation sort preserves source order exactly.
- Equal sort keys retain their relative order in the current source, including after source moves and incremental merges.
- Incremental and complete rebuilds use the same group-identity and ordering-key
  derivation and therefore publish the same final row and section order.

## Derived text keys

Grouped text has a locale-independent Unicode-caseless identity key, an
article-adjusted group-order key, and, when configured, a locale ordering key.
Ordinary authored textual sort terms receive a locale ordering key only when an
interactive policy is present; otherwise they compare the same Unicode
default-folded ordering input as deterministic UTF-8 bytes. Missing values and
typed numeric fields retain their existing comparators.

The policy writes into caller-owned reusable storage. The projection
materializes and interns each key before sorting; comparators perform only
length-aware binary comparisons and never call ICU. A flat presentation with
no sort does not manufacture ordering keys. A policy failure after library text
admission is an invariant failure rather than permission to mix locale and byte
keys in one projection.

A newly created view or detached playback projection receives the current policy from runtime composition.
Live locale replacement updates the projections still owned by `ViewService`, rebuilding their order and publishing a reset; each updated projection retains shared ownership of the replacement.
An already captured detached playback projection is not in that update set and retains its previous ordering policy, so a language change does not reorder an ongoing playback conversation.
A newly constructed playback projection captures the policy current at its construction.

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
Source invalidation clears rows, lookup indexes, group sections, and arena-backed views before publishing one terminal invalidation batch and no later reset.

## Arena rebasing

Sort, group-identity, and locale-order strings are immutable length-aware views
in a `StringArena`.
Incremental updates schedule a full rebase when allocated arena bytes reach twice the post-rebuild baseline with a 64 KiB floor, or touched-row churn reaches 25 percent with a 256-row floor.
The rebase releases all old view holders before discarding the arena and rebuilding.

## Failure and lifetime

Storage failures are not translated into source invalidation; an exception escaping source consumption is fatal at
the owning source-signal boundary.
Internal delta and mirror violations require fail-fast contracts rather than a recovery reset.

**Current implementation discrepancy:** `TrackListProjection::Impl` retains rebuild/reset fallbacks when incremental maintenance, derived edit-script replay, or public-range validation fails.
These branches in `tryApplyIncrementalBatch`, `publishSortedSourceBatch`, and `handleSourceBatch` do not establish the fail-fast guarantee for every internal divergence. Their reachability and treatment need a code/contract resolution; they are not permission for producers to publish malformed deltas.

Projection delivery is synchronous on the callback side.
A lease pins the source identity until the projection releases it, and the projection subscription releases before its source owner.

## Implementation map

- [`TrackListProjection.h`](../../../app/include/ao/rt/projection/TrackListProjection.h) defines the concrete projection, snapshots, and public delta values.
- [`TrackListProjection.cpp`](../../../app/runtime/projection/TrackListProjection.cpp) owns incremental maintenance, grouping, and arena rebasing.
- [`TrackProjectionEditScript.cpp`](../../../app/runtime/projection/TrackProjectionEditScript.cpp) adapts the shared edit algebra.

## Test map

Track-list projection tests under [`test/unit/runtime/projection/`](../../../test/unit/runtime/projection) prove lifecycle, sequential deltas, sorting, grouping, incremental equivalence, arena rebase, mutation behavior, and scale behavior.

## Related documents

- [Library architecture](structure.md)
- [Track sources](track-source.md)
- [Track-list presentation](../presentation/track-presentation.md)
- [Track preset reference](../../reference/presentation/track-preset.md)
- [Runtime track field catalog](../../reference/library/model/track-field.md)
