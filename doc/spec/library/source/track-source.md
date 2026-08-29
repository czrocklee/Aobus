---
id: library.track-source
type: spec
status: current
domain: library
summary: Defines ordered track sources, source leases, delta batches, caches, and source dependency behavior.
---
# Track sources

## Scope

This specification defines the observable ordered-membership and incremental-update contracts of track sources.
It owns leases, source identity, saved-List expression and rank composition, source edit algebra, cache rebinding, and invalidation.

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
- **Raw order** is one saved List's persisted unique `orderTrackIds` sequence, including hidden ranks.
- **Effective List order** is matching ranked members followed by matching unranked members in filtered-parent order.

## Invariants

- Every published regular batch transforms the subscriber's preceding sequence into the source's already-installed final sequence.
- Range coordinates are sequential: each operation is interpreted in the state produced by preceding operations in the batch.
- Reset and invalidation are singleton batch kinds and never mix with regular edits.
- Invalid or duplicate identities in a unique source state are programmer errors.
- Invalidation publishes at most once; destruction or cache teardown alone is not semantic invalidation.
- A lease pins the exact source identity and every upstream dependency needed for its subscription lifetime.
- A List expression determines membership and a saved order determines rank; neither field selects a persisted List kind.
- An empty List expression is the identity predicate `true`.

## Source kinds

### All tracks

The all-tracks source contains every stored track id in its canonical source order.
It consumes committed track insertions, deletions, and metadata updates and publishes one sequential batch per changeset.
Runtime factories construct it only after `MusicLibrary::open()` has validated every persisted Track pair and required dictionary reference.
They complete one initial full reload before exposing `CoreRuntime` or `AppRuntime`, so consumers never observe an empty or partial bootstrap followed by repair.

### Saved Lists

Every saved List uses one fixed pipeline:

```text
parent source
  -> SmartListSource(local expression)
  -> ListOrderSource(raw order)
  -> CachedListSource stable shell
```

`SmartListSource` preserves the stable subsequence of parent order that matches the local expression.
An empty expression matches all upstream tracks.
An invalid expression exposes empty membership and an expression error without invalidating sibling sources.
The error identifies the saved List whose stored expression failed.
A child whose own expression is valid but whose parent chain contains that failure also exposes empty membership and propagates the originating parent error unchanged.

For filtered membership `M` and raw order `R`, `ListOrderSource` exposes:

```text
ranked   = [id in R where id is in M]
unranked = [id in M where id is not in R]
effective = ranked followed by unranked
```

An id in `R` but not `M` remains a hidden rank.
If it later re-enters membership, it returns according to its position in `R`.
New matching ids enter the unranked tail in filtered-parent order.
A parent reorder changes only unranked-tail order; ranked members keep raw-order precedence.

Exact raw-order scripts are applied without a reset fallback.
A move uses descending removals followed by one insertion.
A change affecting only hidden ranks updates raw order without publishing an effective source batch.
A reset clears the overlay and restores filtered-parent order.

Expression syntax and per-track truth belong to the [predicate language](../../../reference/query/predicate-language.md) and [predicate evaluation](../../query/predicate-evaluation.md) contracts.

`SmartListEvaluator` unions the plans' access profiles for each batch and maps that semantic requirement to one physical TrackStore mode: `NoTrackData` and `HotOnly` use `Hot`, `ColdOnly` uses `Cold`, and `HotAndCold` uses `Both`.
`NoTrackData` still uses the least expensive concrete row side because source traversal and point updates must confirm Track existence, while the predicate itself reads no Track fields and TrackStore has no no-row mode.
For a metadata-only batch, the evaluator reads and evaluates only the touched tracks, derives membership edits against the installed buckets, and installs every bucket's final state before publishing any one bucket.
Structural insert, remove, and move batches retain the general rebuild path.
Membership transitions are published as one atomic batch after the final state is installed.

### Ad-hoc filters

`TrackSourceCache` acquires ad-hoc smart sources by `SourceSpec`, consisting of a base list and expression.
Equal specs share one weak-cached source identity while leased.
Before creating a new ad-hoc source, the cache removes every expired weak entry, including entries for unrelated specs; live identities remain shared and untouched.
An ad-hoc source reports its own expression error first; when its expression is valid, it propagates any error from its saved-List base source.

## Cache and dependency behavior

The cache owns all-tracks, List source shells, expression evaluators, rank overlays, and List dependency links.
Cached list identities are stable shells that can rebind an updated implementation.
The cache retains each acquired list identity until that list is deleted or the cache is destroyed.
Ad-hoc filtered sources are weak-cached and may be rebuilt after their last lease is released.

A list definition update may rebind its implementation while preserving the shell identity.
Deleting a list invalidates its source and dependent chain terminally.
Recreating the same numeric list id creates a new source identity; an old invalidated lease never revives.

For each committed library changeset the cache applies deletions, raw order changes, List definition upserts, and track metadata changes in the order required to expose one coherent derived result.
Reentrant mutations are rejected while the cache applies a committed revision.
Only refresh requests discovered during that application are queued and drained afterward, so observers never see a half-rebound graph.
After the cache applies the revision as the publication replica, live views recompute source errors from their retained leases.
Repairing an invalid saved ancestor therefore clears the contextual error in every affected descendant view without rebuilding that view.

## Edit algebra

`delta::RegularTrackEditScript` is the dependency-neutral remove/insert/update representation shared by library changes, sources, and projections.
`TrackSourceDelta` is a variant of that regular script, `SourceReset`, or `SourceInvalidated`; reset and invalidation remain singleton alternatives.

`IndexedTrackSequence` applies update-only scripts and a bounded small structural script in place, then repairs the affected index suffix once after the final sequence is installed.
A larger multi-range script uses the linear edit reducer and rebuilds the index once, preserving O(n + k) work instead of repeatedly shifting and reindexing the remaining sequence.
Malformed coordinates, empty ranges, divergent reducer state, and a final sequence inconsistent with the installed source are fail-fast programming errors rather than recoverable cache-healing events.

## Failure and lifetime

Expected query compilation failures remain recoverable at their owning boundary.
Stored expression failures remain application source state rather than library-integrity faults: views expose the contextual error with an empty projection, playback launch returns it as `FormatRejected`, and a mutation that must interpret a stored parent expression returns contextual `FormatRejected` before committing any change.
Library read failures are exceptional and are not translated while consuming a source update.
Source handlers are ordinary callables; the owning signal emission diagnoses and aborts an escaping exception.
Delta-shape and internal-mirror violations use fail-fast contracts.

The cache is the library's [change-publication replica](../runtime/change-publication.md).
It applies each committed revision before phase-two observers run. The publication owner contains replica exceptions
and treats an escape as fatal rather than exposing a recoverable replica transaction.
Invalidation clears each source snapshot before publishing its terminal event, so an outstanding lease cannot read old size or ids.

Source delivery is synchronous on the callback side.
Subscriptions release before their source or changes owner; source destruction disconnects subscribers but is not itself a semantic invalidation event.

## Implementation map

- [`TrackSource.h`](../../../../app/include/ao/rt/source/TrackSource.h), [`TrackSourceDelta.h`](../../../../app/include/ao/rt/source/TrackSourceDelta.h), and [`TrackSourceLease.h`](../../../../app/include/ao/rt/source/TrackSourceLease.h) define source identity and batches.
- [`TrackSourceCache.h`](../../../../app/include/ao/rt/source/TrackSourceCache.h) exposes acquisition and error lookup while its PImpl owns cache and dependency composition.
- [`CachedListSource.h`](../../../../app/runtime/source/CachedListSource.h) retains stable saved-List identity and resolves local or inherited source errors.
- Source-private [`SmartListSource.h`](../../../../app/runtime/source/SmartListSource.h), [`SmartListEvaluator.h`](../../../../app/runtime/source/SmartListEvaluator.h), [`IndexedTrackSequence.h`](../../../../app/runtime/source/IndexedTrackSequence.h), and [`ListOrderSource.h`](../../../../app/runtime/source/ListOrderSource.h) own expression membership, indexed updates, and rank-overlay behavior.

## Test map

- [`TrackSourceCacheTest.cpp`](../../../../test/unit/runtime/source/TrackSourceCacheTest.cpp) proves cache identity, expired ad-hoc pruning, dependency composition, and contextual propagation of an invalid stored ancestor expression through saved and ad-hoc sources with empty membership.
- Source tests under [`test/unit/runtime/source/`](../../../../test/unit/runtime/source/) prove in-place indexed edits, update-only smart-list membership transitions, edit validation, leases, expression membership, ranked/unranked order, hidden-rank recovery, reentrancy, and mutation-storm equivalence.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Track expression architecture](../../../architecture/track-expression.md)
- [Predicate evaluation](../../query/predicate-evaluation.md)
- [Predicate language](../../../reference/query/predicate-language.md)
- [Library change publication](../runtime/change-publication.md)
- [Track-list projection](../projection/track-list.md)
- [Playback architecture](../../../architecture/playback.md)
