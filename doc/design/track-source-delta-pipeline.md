# Track Source Delta Pipeline

The runtime library pipeline represents committed change as revisioned values.
Storage writes, source membership, smart/manual list evaluation, and live
projections share one edit algebra and one debug-contract policy for internal
invariants.

## Transaction publication

Every `MusicLibrary` write transaction increments the library revision meta
record in-band. A read transaction therefore identifies the exact committed
snapshot it sees. After commit, producers submit one `LibraryChangeSet` with
that revision. `LibraryChanges` holds out-of-order submissions and publishes
them on the callback executor in revision order.

The change set separates inserted, deleted, and metadata-mutated tracks; list
identity changes; detailed manual-list operations; and full-library reset.
`TrackSourceCache` consumes each set in this local order: deletions, collection
changes, manual content, list upserts, then metadata updates.

## Edit algebra and indexed sequences

`delta::RegularTrackEditScript` is the dependency-neutral representation for
ordered remove, insert, and update ranges. Reset and invalidation are terminal
source/projection outcomes, not regular scripts. Source and projection adapters
own type conversion and layer-specific validation.

`IndexedTrackSequence` owns the source layer's repeated ordered-ID plus hash
index structure. Applying a regular script performs one O(n+k) merge and one
index rebuild. Smart-list evaluation applies the upstream script once, reads
only inserted/updated track records, performs one final-order membership walk
per dependent list, and derives outgoing scripts through the shared kernel.
Manual stored and effective orders use the same sequence owner.

## Live projection order maintenance

`LiveTrackListProjection` keeps both the source-order ID snapshot and the
presentation-order entries. A regular source script is first replayed against
the ID snapshot and checked against the source's final order. The projection
then retains untouched entries, reads and rebuilds only inserted or
metadata-updated entries, sorts that touched subset, and merges it with the
already-sorted retained subset. Source-ordered projections rebuild their entry
vector from the validated ID snapshot and do not read track records for
metadata-only updates.

The row lookup index and group-section spans are rebuilt once in linear time
per batch. The shared edit kernel still derives, applies, and validates every
published projection script. A section identity/order/metadata change is
published as `ProjectionReset`; membership-count changes can remain regular
row deltas.

Sort and group strings are immutable views into `StringArena`. Incremental
updates therefore accumulate arena blocks until either allocated bytes reach
twice the post-rebuild baseline (with a 64 KiB floor) or touched-row churn
reaches 25% of a large projection (with a 256-row floor). The next full rebase
discards all view holders before releasing the arena and rebuilding from the
source. Source reset and presentation changes are also full-rebuild points.

## Programming invariants

Malformed edit coordinates, duplicate identities, divergent reducer mirrors,
and invalid source publication batches indicate bugs in Aobus rather than
recoverable operational failures. These conditions use `gsl_Assert`.

The project enables gsl-lite fail-fast assertions in Debug builds and disables
contract evaluation in non-Debug builds. There is no delta failure sink or
automatic cache-healing state machine. Expected failures such as invalid user
input, query errors, and storage errors continue to use their existing
`Result` or exception contracts.

Kernel property tests and the mutation-storm oracle exercise valid transitions
and compare every live source and projection against a ground-truth recompute.

## Source ownership and ad-hoc filters

`TrackSourceLease` is the non-null ownership handle. Cached list identities are
stable shells that can rebind their implementation. Ad-hoc filters are acquired
through `TrackSourceCache` with `SourceSpec` (base list plus expression); equal
specs share one weak-cached smart source. View projections and playback cursor
sessions therefore share evaluation and invalidation semantics.

Live projections remain synchronous. Incremental order maintenance reduced a
single metadata update through three 50k-track smart-list projections from
about 43 ms to about 15 ms on the reference benchmark; a direct projection
dropped from about 14 ms to about 4 ms. Async smart-list evaluation remains
gated off because its measured cost is still within the current callback
budget.

See also [playback cursor](playback-cursor.md) for how playback consumes source
leases and [runtime/UI-model boundary](runtime-uimodel-boundary.md) for layer
ownership.
