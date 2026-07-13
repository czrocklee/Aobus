---
id: library.track-source
type: spec
status: current
domain: library
summary: Defines ordered track sources, source leases, revisioned deltas, caches, and source dependency behavior.
---
# Track sources

## Scope

This specification defines the observable ordered-membership and incremental-update contracts of track sources.
It owns leases, source identity, manual and smart membership, source edit algebra, cache rebinding, and invalidation.

Projection behavior belongs to the [track-list](../projection/track-list.md) and [track-detail](../projection/track-detail.md) specifications.
Changeset production belongs to [library change publication](../runtime/change-publication.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
Its public boundary is `app/include/ao/rt/source/`, its implementation is `app/runtime/source/`, and it may consume `ao::rt::library` values and core library/query facilities without depending on projections, UIModel, or frontends.

## Terminology

- **Source order** is the ordered `TrackId` sequence exposed by one `TrackSource`.
- **Regular batch** is an ordered sequence of remove, insert, and update ranges.
- **Reset** installs a complete replacement state.
- **Invalidation** is the terminal semantic end of a source identity.
- **Lease** is the non-null shared ownership handle that pins a source and its dependency graph.

## Invariants

- Every published regular batch transforms the subscriber's preceding sequence into the source's already-installed final sequence.
- Range coordinates are sequential: each operation is interpreted in the state produced by preceding operations in the batch.
- Reset and invalidation are singleton batch kinds and never mix with regular edits.
- Invalid or duplicate identities in a unique source state are programmer errors.
- Invalidation publishes at most once; destruction or cache eviction alone is not semantic invalidation.
- A lease pins the exact source identity and every upstream dependency needed for its subscription lifetime.

## Source kinds

### All tracks

The all-tracks source contains every stored track id in its canonical source order.
It consumes committed track insertions, deletions, and metadata updates and publishes one sequential batch per changeset.

### Manual lists

A manual source keeps stored intent separate from effective membership.
Its effective order is the stable subsequence of stored ids currently present in its parent source.
When an upstream track disappears and later re-enters, it returns to its stored position.

Exact manual insert, remove, and move changes are translated without a reset fallback.
A change affecting only currently hidden stored ids updates intent without publishing an effective source batch or advancing its visible revision.
Parent reorder alone does not reorder manual intent.

### Smart lists

A smart source evaluates its expression against its parent source and preserves the matching stable subsequence of parent order.
An empty expression matches all upstream tracks.
An invalid expression exposes an empty membership and an expression error without invalidating sibling sources.

Expression syntax and per-track truth belong to the [predicate language](../../../reference/query/predicate-language.md) and [predicate evaluation](../../query/predicate-evaluation.md) contracts.

Incremental evaluation reads only inserted or updated track records needed by the compiled expression's hot/cold access profile.
Membership transitions are published as one atomic batch after the final state is installed.

### Ad-hoc filters

`TrackSourceCache` acquires ad-hoc smart sources by `SourceSpec`, consisting of a base list and expression.
Equal specs share one weak-cached source identity while leased.

## Cache and dependency behavior

The cache owns all-tracks, list source shells, smart evaluators, and list dependency links.
Cached list identities are stable shells that can rebind an updated implementation.

A list definition update may rebind its implementation while preserving the shell identity.
Deleting a list invalidates its source and dependent chain terminally.
Recreating the same numeric list id creates a new source identity; an old invalidated lease never revives.

For each committed library changeset the cache applies deletions, collection changes, detailed manual content, list upserts, and metadata updates in the order required to expose one coherent derived result.
Reentrant changes queue behind the batch currently being published so observers never see a half-rebound graph.

## Edit algebra

`delta::RegularTrackEditScript` is the dependency-neutral remove/insert/update representation.
The source adapter converts to and from `TrackSourceDeltaBatch` and validates source-specific constraints.

`IndexedTrackSequence` applies one regular script with one merge and one index rebuild.
Malformed coordinates, empty ranges, divergent reducer state, and a final sequence inconsistent with the installed source are fail-fast programming errors rather than recoverable cache-healing events.

## Failure and lifetime

Expected query compilation and library read failures remain recoverable at their owning boundary.
Delta-shape and internal-mirror violations use fail-fast contracts.

Source delivery is synchronous on the callback side.
Subscriptions release before their source or changes owner; source destruction disconnects subscribers but is not itself a semantic invalidation event.

## Implementation map

- [`TrackSource.h`](../../../../app/include/ao/rt/source/TrackSource.h), [`TrackSourceDelta.h`](../../../../app/include/ao/rt/source/TrackSourceDelta.h), and [`TrackSourceLease.h`](../../../../app/include/ao/rt/source/TrackSourceLease.h) define source identity and batches.
- [`TrackSourceCache.h`](../../../../app/include/ao/rt/source/TrackSourceCache.h) owns cache and dependency composition.
- Source implementations under [`app/runtime/source/`](../../../../app/runtime/source/) own all/manual/smart behavior.

## Test map

Source tests under [`test/unit/runtime/source/`](../../../../test/unit/runtime/source/) prove edit validation, leases, cache identity, manual intent, smart membership, reentrancy, and mutation-storm equivalence.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Track expression architecture](../../../architecture/track-expression.md)
- [Predicate evaluation](../../query/predicate-evaluation.md)
- [Predicate language](../../../reference/query/predicate-language.md)
- [Library change publication](../runtime/change-publication.md)
- [Track-list projection](../projection/track-list.md)
- [Playback architecture](../../../architecture/playback.md)
