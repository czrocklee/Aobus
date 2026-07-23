---
id: playback.cursor
type: spec
status: current
domain: playback
summary: Defines live-projection playback succession, cursor anchors, navigation policy, prepared-next correlation, and failure walking.
---
# Playback succession cursor

## Scope

This specification defines current application-level track succession over a runtime-owned live projection.
It owns launch capture, cursor and anchor behavior, next/previous resolution, repeat and shuffle policy, source invalidation, prepared-next correlation, recovery walking, and frontend-neutral succession observations.

Playback ownership and execution domains belong to the [playback architecture](../../architecture/playback.md).
Ordered source membership belongs to [track sources](../library/source/track-source.md), projection delta production belongs to the [track-list projection](../library/projection/track-list.md), presentation ordering belongs to [track-list presentation](../presentation/track-presentation.md), and persisted payload and restore behavior belong to [playback session persistence](session-persistence.md).
The public application boundary coordinates commits without replacing the succession behavior owned here.

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../architecture/system-overview.md) and refines the succession authority in the [playback architecture](../../architecture/playback.md).
Its public snapshot and command values live in [`app/include/ao/rt/playback/`](../../../app/include/ao/rt/playback/), while the `PlaybackSuccession` owner and its implementation live entirely under [`app/runtime/playback/`](../../../app/runtime/playback/).
It consumes `ViewService`, source leases, live projections, library reads, and the private application-transport collaboration surface; it does not depend on UIModel, frontend types, or Core audio policy.

## Terminology

- **Launch spec** is the captured source list, quick-filter expression, and sort order for one playback conversation.
- **Session projection** is the detached live projection owned by that conversation.
- **Current subject** is the valid `TrackId` identifying the audio that is already present, even when that track no longer belongs to the projection.
- **Bound anchor** means the current subject is present at the anchor index.
- **Gap anchor** means the current subject is absent and the anchor index denotes the successor position that remains after removal or filtering.
- **Prepared commitment** correlates one best-effort application successor with an audio lookahead token and its independently maintained anchor.
- **Semantic tuple** is `(source state, current TrackId, has-next, has-previous, resolved successor)`.
- **Terminal succession** means no further source-derived track may be chosen; it does not by itself erase the already-current audio subject.

## Invariants

- The live list governs succession, never the present: membership and order changes do not interrupt, stop, or restart audio that is already current.
- Frontends launch with view and track identities only and never provide an ordered queue or visible-row snapshot.
- One active session owns one source lease and one detached live projection; it owns no second materialized succession vector.
- The launch spec remains fixed for the session, while committed library changes continue to update its projection.
- An active cursor always has a valid current `TrackId`.
- While the source is Live, the anchor index remains in `[0, projection size]`; Bound is strictly below the size and Gap may equal it.
- A complete projection batch is reconciled before cursor state, prepared commitments, or observers see its result.
- Source invalidation is terminal for that source identity; recreating the same numeric list id never revives the session.
- Prepared tokens, audio item ids, cancellation generations, projection positions, and library identities retain separate meanings.
- Cursor and service mutation runs on the runtime callback executor; lower audio callbacks marshal there before changing succession state.

## State model

The public succession state has three source states:

| State | Cursor/session meaning | Command authority |
|---|---|---|
| Inactive | No active cursor session; current and source identities are invalid. Shuffle and repeat preferences remain available. | Succession commands are no-ops. |
| Live | The session projection and cursor are valid succession authority. | Commands use repeat, shuffle, history, and anchor policy. |
| Invalidated | The source identity was deleted; the current subject and frozen anchor remain observable. | No projection or shuffle state may be consulted; only current-track restart remains possible. |

An explicit `clear()` transitions an active succession session to Inactive, disarms lookahead, and preserves a last-restorable cursor snapshot.
It does not stop audio that is already playing.
Terminal exhaustion or unrecoverable sequence failure instead stops transport and then transitions succession to Inactive.

### Live anchor states

While the source is Live, Bound and Gap form an independent anchor axis:

| Anchor | Identity relation | Successor position |
|---|---|---|
| Bound | `currentTrackId` is present at `anchorIndex`. | Sequential next begins at `anchorIndex + 1`. |
| Gap | `currentTrackId` is absent from the projection. | Sequential next begins at `anchorIndex`. |

The current library record may disappear while its decoded audio remains current.
That changes a Bound cursor to Gap without changing the current subject.
An empty Live projection is Gap at zero.

## Commands and transitions

### Launch

`playFromView(viewId, startTrackId)` captures one coherent launch description from `ViewService`.
The captured spec contains the exact source list id, quick-filter expression, and `sortBy` order; grouping, visible fields, redundant fields, and other display state do not become succession context.
An empty sort preserves source order, including the stored relative order exposed by a manual source.

Changing or destroying the originating view after launch does not alter the session.
Changing its filter or presentation affects only a later launch.
Library edits continue to affect the session through its own source lease and projection.

Launch resolution is strict:

- `kAllTracksListId` resolves the permanent virtual All Tracks source.
- Every other list id must resolve that exact list; launch never substitutes All Tracks.
- The view, source, filter, start track, and resolved playback request must all be valid.
- The start track must occur in the captured projection.

Launch prepares the source lease, filter, projection, anchor, and audio request before replacing the accepted session.
Failure before transport accepts the new start leaves the preceding succession and transport state unchanged.
After acceptance, the new session is authoritative and later asynchronous playback failure follows the recovery rules below.

Restore constructs the same launch spec and detached session chain without claiming a view identity.
Its candidate normalization and installation order belong to [playback session persistence](session-persistence.md).

### Projection-batch reconciliation

The cursor consumes the sequential range coordinates, singleton reset, and terminal invalidation batches defined by the [track-list projection](../library/projection/track-list.md).
Projection deltas carry ranges rather than track identities, so the cursor transforms the working anchor by range and reconciles the final identity with `indexOf(currentTrackId)` after the complete batch.

For insertion of `[start, start + count)`:

- Bound advances by `count` when `start <= anchorIndex`.
- Gap advances by `count` only when `start < anchorIndex`; insertion exactly at the gap becomes the next content.

For removal of `[start, end)`:

- when `end <= anchorIndex`, the anchor moves left by `count`;
- when `start <= anchorIndex < end`, the anchor collapses to `start`, and Bound becomes Gap;
- a range strictly after the anchor has no effect.

Update ranges have no positional effect.
After all ranges, identity wins: a present current subject becomes Bound at its actual index; otherwise it remains Gap and clamps to the final size.
A remove-plus-insert move can therefore return directly to Bound without publishing an intermediate Gap.

Reset skips range transformation.
It binds to the current subject when present and otherwise retains a Gap at `min(previous anchor, final size)`.

### Resolution precedence

Policy precedence is:

1. Source invalidation disables every source-derived result.
2. Previous restarts the current track when elapsed time is strictly greater than three seconds.
3. Repeat-one decides forward succession.
4. Shuffle decides forward succession and history-backed previous.
5. Sequential anchor policy decides the remaining commands.

Natural advance and manual next use the same forward resolution.
When no forward successor exists, either command takes the terminal stop path; `hasNext` remains false before that command.
Previous is different: when neither restart nor a predecessor exists, it is a no-op and does not stop playback.

### Live sequential and repeat behavior

- Repeat-one resolves forward succession to the current subject without consulting projection membership, including a Gap or empty Live projection.
- Sequential next resolves Bound to `anchorIndex + 1` and Gap to `anchorIndex`.
- Repeat-all wraps forward to row zero only when the projection is non-empty.
- Sequential previous resolves to `anchorIndex - 1` when the anchor is greater than zero.
- Repeat-all wraps previous to the last projected row only when the projection is non-empty.
- Repeat-all never turns an empty projection into a successor.
- `hasNext` and `hasPrevious` report the exact availability implied by these rules and the current restart policy.

The previous restart threshold is strict: exactly 3000 ms remains unavailable and elapsed time greater than 3000 ms enables restart.
Pause, resume, final seek, current-track change, session replacement, and shutdown reschedule or cancel the executor-affine deadline.
The deadline returns through a cancellation-checked callback-executor hop, so an obsolete task cannot change availability; a separate synchronization revision detects only synchronous reentrancy from availability publication.

### Source invalidation

`ProjectionSourceInvalidated` changes Live to Invalidated, clears shuffle history and its sticky candidate, and asks transport to disarm prepared-next.
The current audio continues.
The frozen anchor remains available for a coherent session snapshot but is no longer succession authority.

While Invalidated:

- `hasNext` is false and no resolved successor exists;
- natural end and manual next stop transport and finish the sequence, regardless of repeat mode;
- previous restarts the current subject only after the elapsed threshold and is otherwise a no-op;
- pause, resume, seek, and output control remain transport operations;
- recoverable track failure never walks the stale projection;
- no command queries the projection or shuffle history.

Runtime shutdown may destroy the graph without publishing a user-visible invalidation.

### Shuffle

Shuffle forward selection is random and memoryless across accepted transitions, but its pending candidate is sticky while it remains eligible.
Unrelated projection changes do not reroll it.
Departure, a relevant repeat/shuffle mode change, explicit invalidation, or a current preparation/playback failure rerolls it.
A cancelled, superseded, or stale lookahead completion does not mutate the sticky candidate.

A Bound current is excluded from the forward candidate set; a Gap current excludes nothing.
When no alternative exists, repeat-all may select the sole projected Bound current.
Otherwise an empty candidate set has no successor.

The transient history records the actual navigation path:

- every successful transition to a different track pushes the track being left, including sequential previous;
- restart, same-track replay, and history-backed previous do not push;
- history previous pops until it finds a projected entry different from the current subject;
- an entry that fails to start remains discarded, and the failure walk continues in the history direction;
- `hasPrevious` scans for an eligible entry without mutating history;
- repeat-all never synthesizes history;
- history retains at most the newest 64 entries and clears on source invalidation.

Turning shuffle off resumes sequential navigation from the continuously maintained current anchor.
No-repeat-until-cycle-complete is not current policy.

### Prepared next

Prepared next is a best-effort audio commitment, not a transaction with live membership.
Every active or retired commitment carries a nonzero opaque token, an issuance generation, a successor `TrackId`, and its own Bound or Gap anchor.
Projection batches maintain each commitment independently with the same anchor rules as the current subject.

The resolved successor determines the active commitment.
If the successor changes, transport replaces lookahead; output-device changes and final seek may replace it with a new token even when the successor TrackId is unchanged.

Replacement and cancellation follow proof rather than timing assumptions:

- an exact disarm acknowledgement removes that token immediately;
- an unacknowledged replaced token becomes retired because its transition may already be queued;
- a cancellation barrier removes only commitments whose issuance generation it proves unreachable;
- an exact active or retired winner supplies its maintained anchor and closes the whole competing-token window;
- an unknown non-null token is stale and must not be reclassified as an explicit start.

An explicit start has no prepared token.
While Live, it anchors by current projection identity or by the clamped preceding gap.
While Invalidated, it may update only the current subject and leaves the frozen anchor non-authoritative.

Invalidation retires an unacknowledged prepared token.
If that already-started transition wins the race, its track becomes current and may play out, but the cursor remains Invalidated and prepares no further successor.

### Clear and mode changes

`clear()` removes succession authority without stopping present transport.
`next()`, `previous()`, and `clear()` are no-ops when no session is active.
Public observer reentrancy is serialized by the owning `PlaybackService` commit boundary rather than rejected by succession.

Shuffle and repeat preferences exist even while Inactive and are inherited by the next launch.
Setting a mode to its current value is a no-op.
An actual mode change invalidates or replaces the sticky/prepared forward candidate as needed and publishes its dedicated mode event.

## Failure and cancellation

Launch failures are atomic before acceptance as described above.
Expected missing view, source, projected start, invalid filter, missing library record, and audio preparation errors return recoverable failures.

Navigation and recoverable current-track open/decode failure walk candidates in the requested direction against the latest live projection.
Each candidate is resolved immediately before its attempt.
Shuffle-forward failure excludes attempted candidates before rerolling; shuffle-previous failure continues through popped history rather than falling through to sequential previous.
Current shuffle lookahead preparation failure silently excludes that candidate from the preparation attempt chain, rerolls, and prepares another eligible successor.
The chain attempts at most three candidates; a non-shuffle failure, exhausted chain, or absence of another eligible candidate leaves recovery to the normal boundary path.
Lookahead cancellation, supersession, and `Conflict` completion clear only matching pending preparation state and never reroll.
Acceptance and completion carry a weak reference to the pending lookahead object and may mutate succession only while that exact object remains current.

Three consecutive unplayable candidates terminate transport and succession.
A successful start resets the failure streak.
A failure after source invalidation, or any non-recoverable device or route failure, stops without a recovery walk.
Prepared-successor failure is accepted only for a token owned by the current session, removes that commitment, and re-prepares from current live policy when possible.

Stopping, explicit session replacement, and cancellation barriers retire prepared correlations according to their proof generation.
Queued callbacks from a replaced session cannot become explicit succession transitions.

## Persistence and versioning

The launch spec, current subject, anchor index, shuffle mode, and repeat mode are durable listening intent.
Prepared commitments, source validity, projection rows, sticky shuffle candidates, navigation history, and audio generations are transient and are never serialized.

An active session supplies its live cursor snapshot.
Before clear, stop, exhaustion, or invalidation removes succession authority, the runtime captures an immutable last-restorable cursor snapshot.
Composite persistence rejects a cursor/transport current-subject mismatch instead of combining different playback generations.

The restore matrix and event-driven save timing belong to [playback session persistence](session-persistence.md); exact fields and schema compatibility belong to the [session-state reference](../../reference/playback/session-state.md).
Application coordination and persistence installation belong to the `PlaybackService` commit boundary; cursor policy remains in succession.

## Frontend observations

Internal `PlaybackSuccessionState` exposes source state, current and source identities, next/previous availability, resolved successor, and modes without exposing projection rows or prepared tokens.

`onChanged` publishes synchronously on the callback executor only when the semantic tuple changes.
Anchor movement, membership churn, or preparation replacement that leaves the tuple unchanged is not a semantic event.
Source invalidation, current-track transition, successor change, and crossing the previous-restart threshold publish only when they change that tuple.

Shuffle and repeat have dedicated events for actual mode changes because a mode can change without changing the semantic tuple.
No-op commands publish neither semantic nor mode events.
Observers are observational and do not choose succession policy.

## Implementation map

- [`PlaybackSuccession.h`](../../../app/runtime/playback/PlaybackSuccession.h) defines the internal succession owner, state, commands, and subscriptions; [`PlaybackSnapshot.h`](../../../app/include/ao/rt/playback/PlaybackSnapshot.h) defines its public read-only projection.
- [`PlaybackLaunchSpec.h`](../../../app/include/ao/rt/PlaybackLaunchSpec.h) defines captured launch context.
- [`PlaybackSuccession.cpp`](../../../app/runtime/playback/PlaybackSuccession.cpp) coordinates launch, commands, failure walking, transport collaboration, and internal publication.
- [`PlaybackCursor`](../../../app/runtime/playback/PlaybackCursor.h) owns the pure source/anchor/mode state machine and semantic tuple.
- [`PlaybackCursorSession`](../../../app/runtime/playback/PlaybackCursorSession.h) owns the lease, detached projection, cursor policy, and observation lifetime.
- [`ProjectionAnchor`](../../../app/runtime/playback/ProjectionAnchor.h), [`ShuffleHistory`](../../../app/runtime/playback/ShuffleHistory.h), [`PreparedNextRegistry`](../../../app/runtime/playback/PreparedNextRegistry.h), and [`PlaybackRestartDeadline`](../../../app/runtime/playback/PlaybackRestartDeadline.h) own the focused policy mechanisms.

## Test map

- [`PlaybackCursorModelTest.cpp`](../../../test/unit/runtime/playback/PlaybackCursorModelTest.cpp) proves state precedence, semantic/restorable mutation effects, complete-batch observation, and model-based anchor invariants.
- [`ProjectionAnchorTest.cpp`](../../../test/unit/runtime/playback/ProjectionAnchorTest.cpp) proves insertion/removal boundaries, move reconciliation, reset, empty gaps, and range invariants.
- [`ShuffleHistoryTest.cpp`](../../../test/unit/runtime/playback/ShuffleHistoryTest.cpp) proves sticky candidates, eligibility, path history, failed-pop behavior, invalidation, and the 64-entry bound.
- [`PreparedNextRegistryTest.cpp`](../../../test/unit/runtime/playback/PreparedNextRegistryTest.cpp) proves active/retired replacement, independent anchors, exact disarm, winner resolution, invalidation races, and cancellation barriers.
- [`PlaybackRestartDeadlineTest.cpp`](../../../test/unit/runtime/playback/PlaybackRestartDeadlineTest.cpp) proves the strict threshold, queued-task cancellation, synchronous reentrancy, pause/resume/seek control, session replacement, and shutdown.
- [`PlaybackSuccessionTest.cpp`](../../../test/unit/runtime/PlaybackSuccessionTest.cpp) proves launch atomicity, detached view context, live membership, repeat, prepared transitions, failure walking, and internal observations; [`PlaybackServiceTest.cpp`](../../../test/unit/runtime/PlaybackServiceTest.cpp) protects public reentrancy ordering and projection.
- Source and projection suites linked from their owning specifications prove the ordered live input contract consumed here.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Track sources](../library/source/track-source.md)
- [Track-list projection](../library/projection/track-list.md)
- [Track-list presentation](../presentation/track-presentation.md)
- [List model reference](../../reference/library/model/list.md)
- [Track presentation preset reference](../../reference/presentation/track-preset.md)
- [Playback session persistence](session-persistence.md)
- [Playback session state reference](../../reference/playback/session-state.md)
