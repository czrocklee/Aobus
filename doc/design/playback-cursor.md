# Playback Cursor

> **Status: current architecture.** This document defines the implemented
> live-projection playback-succession model. The cross-layer ownership contract
> is summarized in
> [Runtime and UIModel Boundary](runtime-uimodel-boundary.md).

Aobus has no dedicated playlist entity. A list is the only membership model:
a smart list defines membership by filter, and a manual list by stored track
IDs (see `ListView::isSmart()`). Playback therefore follows a list live instead
of copying it.

## Motivation

Playback uses a **cursor over a runtime-owned live projection**. The same
`TrackSource` and `LiveTrackListProjection` machinery that backs browsing views
backs playback succession. A view and an active playback session may each own
a projection, but playback owns no second track-ID vector beside its
projection, so it has no divergent queue snapshot. Frontends provide only a
view identity and starting track identity; they never decide runtime succession
by materializing visible rows.

## Zeroth principle

**The list governs succession, never the present.** Membership and order
changes affect only how `next`, `previous`, repeat, and prepared-next resolve.
They never interrupt, stop, or restart audio that has already become current.

Deleting a source list is a terminal succession event, not an exception to
this principle: the current track still plays out, but nothing follows it.
Deleting the underlying track file is not a membership event; replay or resume
failure surfaces through the normal `PlaybackService` failure path.

## Launch contract

Frontend launch uses `playFromView(viewId, startTrackId)`.

- Frontends pass no ordering data. `ViewService` already owns the authoritative
  list ID, quick-filter expression, and presentation. The runtime captures
  them synchronously on its executor, so the combination is coherent.
- The captured `PlaybackLaunchContext` contains a list ID, a quick-filter
  expression, and `TrackOrderSpec`. The start track is command input, not part
  of the durable context.
- `TrackOrderSpec` contains only `sortBy`. A non-empty sort uses the
  projection's TrackId tie-break; an empty sort preserves source positions.
  `groupBy` creates display sections but does not independently reorder rows;
  any intended group ordering must already be encoded in `sortBy`. Visible and
  redundant fields are display state and never become playback or
  persisted-session state.
- The order context is fixed at launch: **what you see is what you play.**
  Subsequent view interaction -- changing presentation, editing the quick
  filter, navigating away, or closing the view -- does not affect playback.
  Re-launching adopts the new context. Library changes do affect playback
  through the session's own projection.

Launch uses strict source resolution:

- `kAllTracksListId` explicitly resolves the permanent virtual All Tracks
  source.
- A non-virtual list ID resolves only when that exact list exists.
  `TrackSourceCache::acquire()` returns `NotFound` for a missing list; ordinary
  launch never silently substitutes All Tracks.
- An unknown view, missing list, invalid quick filter, or start track absent
  from the captured projection returns the corresponding error.

Launch is prepare/commit. The runtime first captures and validates the context,
acquires the source, builds the candidate projection, resolves the start track,
and constructs its `PlaybackRequest`. A failure before `PlaybackService`
accepts that request leaves the existing playback session unchanged. Once the
request is accepted, the new cursor session becomes authoritative; later
asynchronous playback failures follow its normal recovery policy.

The restore path constructs the same `PlaybackLaunchContext` internally. It
does not pretend to own a view.

## Session ownership and source invalidation

The playback session owns a chain, not just a projection:

```text
TrackSourceLease                         (session-owned)
  -> base TrackSource dependency graph  (shared, lease-pinned)
    -> optional quick-filter source     (session-owned)
      -> LiveTrackListProjection        (session-owned)
        -> playback cursor
```

A `TrackSourceLease` pins the complete dependency graph, not only its leaf.
Closing a view or evicting a cache entry therefore cannot leave a session with
dangling parent references. The lease is released when the playback session
ends.

Object lifetime and domain validity are separate contracts:

- Cache eviction drops cache ownership but does not invalidate an outstanding
  lease.
- Deleting a library list explicitly invalidates its source and every
  descendant source before cache ownership is released. Invalidation is
  delivered exactly once even while leases keep the C++ objects alive.
- A derived manual, smart, or quick-filter source propagates upstream
  invalidation as its own terminal invalidation. It must not translate it into
  an ordinary empty reset.
- Recreating a list with the same ID does not resurrect an invalidated session;
  the listener must launch again.
- C++ destruction is cleanup, not the semantic signal for list deletion.

All Tracks is valid for the lifetime of the runtime. Runtime shutdown tears the
graph down without publishing a user-visible source invalidation.

The session projection is detached from `ViewService`. If
`LiveTrackListProjection` retains its current view-oriented constructor, it
explicitly accepts `kInvalidViewId` for this use and no view event is published
for it. The cursor never invents or registers a synthetic view.

## Projection contract

The cursor consumes `TrackListProjectionDeltaBatch` under these guarantees:

1. **Atomic projection operations.** One upstream observer callback or one
   projection recomputation that changes rows publishes at most one batch. A
   writer transaction may legitimately cause several source callbacks and
   therefore several batches. Within one operation, a move is one batch
   containing remove plus insert; the current ungrouped
   `removeEntry()`/`insertEntry()` pair must not publish two batches.
2. **Sequential coordinates.** Deltas in a batch apply in order. Each delta is
   expressed in the coordinate space produced by preceding deltas in that
   batch. A row range is the half-open interval
   `[start, start + count)`.
3. **Singleton reset.** `ProjectionReset` is the only delta in its batch. It
   replaces the complete ordering and provides no old-to-new index mapping.
4. **Terminal invalidation.** `ProjectionSourceInvalidated` is the only delta
   in the final batch when the source chain is invalidated. No projection
   batch follows it.

Deltas remain range-based and carry no track IDs. Anchor handling therefore
uses ranges plus post-batch `indexOf()` queries; the cursor never depends on
per-track delta identity.

## Cursor state

An active cursor always has a non-invalid `currentTrackId`. During playback the
corresponding library record may be deleted, but the ID still denotes the
already-current audio subject. Restore never creates an active cursor without
a resolvable current target; the missing-track rules below choose a replacement
or discard the session. Stopped or explicitly cleared playback has no cursor
session and lies outside this state machine; it is not a third anchor state.

The cursor has two independent state axes.

### Source state

| State | Meaning |
| --- | --- |
| **Live** | Projection deltas and succession commands are valid. |
| **Invalidated** | The source list was deleted. The current track may finish, but projection content and history are no longer succession authority. |

`ProjectionSourceInvalidated` transitions Live to Invalidated, retires or
disarms prepared commitments, and clears shuffle history. Source validity is
not serialized, so invalidation alone does not change persistence revision.
The anchor is frozen for a coherent session snapshot, but terminal command
resolution never consults it and no further projection query is made.

### Anchor state

While the source is Live, the cursor is in one of two anchor states:

| State | Meaning | Position authority |
| --- | --- | --- |
| **Bound** | `currentTrackId` is in the projection | `indexOf(currentTrackId)` after every batch; `anchorIndex` is a cache |
| **Gap** | The current track is outside the projection | `anchorIndex` is the old position gap and points at the successor that slid into it |

The valid anchor range is `[0, projection.size()]`. A Bound anchor is always
strictly below `size()`; a Gap may equal `size()`.

### Batch processing

A regular delta batch is consumed atomically. The cursor maintains a working
anchor state and index while applying the batch in order, then reconciles by
identity, then -- and only then -- publishes state or changes prepared-next.
Nothing observes a mid-batch state.

For an insert range with start `s` and count `n` in the pre-insert coordinate
space:

- Bound: if `s <= anchorIndex`, add `n`. Inserting at the current row pushes
  the current track right.
- Gap: if `s < anchorIndex`, add `n`. Inserting exactly at the gap leaves it
  fixed, and the first inserted track becomes the successor.
- Otherwise the anchor is unchanged.

For a remove range `[s, e)` where `e = s + n`:

- If `e <= anchorIndex`, subtract `n`.
- If `s <= anchorIndex < e`, collapse the anchor to `s`. If the working state
  was Bound, it becomes Gap because the current row was removed.
- If `s > anchorIndex`, leave the anchor unchanged.

An update range has no positional effect. After all deltas:

- If `indexOf(currentTrackId)` is present, identity wins: enter Bound and
  refresh `anchorIndex` from it.
- Otherwise enter or remain in Gap and clamp `anchorIndex` to `size()`.

A remove-plus-insert move of the current track may make the working state Gap,
but the final identity reconciliation returns it directly to Bound without a
public intermediate transition.

`ProjectionReset` skips range transformation: if the current track exists in
the new content, enter Bound at its index; otherwise enter Gap at
`min(previousAnchorIndex, size)`.

An empty Live projection produces Gap at 0. It has no list successor, including
under repeat-all. Repeat-one remains a separate current-track policy described
below.

## Command resolution

Policy precedence is explicit:

1. Source invalidation is terminal for succession and overrides repeat and
   shuffle.
2. The elapsed-time previous command may restart the current track without
   consulting succession state.
3. With a Live source, repeat-one controls natural advance and `next()`.
4. Shuffle controls forward selection and history-backed previous.
5. Otherwise commands use sequential anchor resolution.

For an Invalidated source:

- `hasNext() == false`.
- Pause, resume, seek, and output controls continue to address the current
  track; invalidation ends succession, not ordinary control of the present.
- Natural track end stops playback and posts the terminal sequence notification,
  even under repeat-one or repeat-all.
- Manual `next()` takes the same terminal stop path immediately.
- `previous()` may restart the current track only when elapsed time exceeds the
  restart threshold; otherwise it is a no-op. `hasPrevious()` reflects exactly
  that restart availability.
- A playback failure stops through the service/terminal notification path; it
  never starts a recovery walk over the invalidated projection.
- No command reads the stale projection or shuffle history.

For a Live source:

- Repeat-one replays the current track without consulting membership. This
  includes an empty projection or a current track in Gap. If the current
  library record or file can no longer be opened, the ordinary playback
  failure path applies.
- Sequential `next()` resolves Bound to `index + 1` and Gap to
  `anchorIndex`. Repeat-all wraps to 0 only when the projection is non-empty.
- `previous()` first restarts the current track when elapsed time exceeds the
  threshold. Otherwise sequential previous resolves Bound to `index - 1` and
  Gap to `anchorIndex - 1`. Repeat-all wraps backward to the last row only when
  the projection is non-empty.
- Shuffle forward and shuffle previous follow the rules in the Shuffle
  section.
- `hasNext()` and `hasPrevious()` mirror these exact command outcomes. In
  particular, repeat-one makes next available while its current target exists
  conceptually, whereas repeat-all never makes an empty projection non-empty.

Forward and backward failure walks are bounded by the consecutive-failure
policy. Each candidate is resolved against the live order immediately before
it is attempted. A successful explicit navigation resets the
consecutive-failure streak.

### Semantic revision and persistence revision

Public observation and durable-state change are separate:

- **Semantic revision** backs `onChanged`. It changes only when the resolved
  tuple `(sourceState, currentTrackId, hasNext, hasPrevious,
  optResolvedSuccessor)` changes. Membership churn that leaves the tuple intact
  is a public no-op. Projection batches are not the only possible trigger:
  current-track and mode changes, source invalidation, and crossing the
  previous-command restart threshold also re-evaluate the tuple.
- **Persistence revision** changes on discrete serialized-intent mutations,
  including `anchorIndex`, launch context, current track, modes, volume/mute,
  and a final seek. Ordinary elapsed-time progress is sampled when a periodic
  or significant-event save occurs; it does not dirty the session on every
  playback clock update. Shuffle history and prepared commitments are not
  serialized and do not by themselves dirty persistence.

The runtime maintains a clean/dirty persistence state. The clean-to-dirty
transition publishes `onPlaybackSessionDirty` as a lifecycle signal; a
frontend coordinator may debounce it and invoke the runtime's composite save
operation, but never inspects or constructs the payload. A successful save
marks the exact saved revision clean. If state changed during the save, the
newer revision remains dirty and schedules another save. Periodic and shutdown
saves remain safety nets, not the only way an anchor change is persisted while
paused. A failed save leaves the revision dirty for the coordinator's normal
retry/backoff path.

All cursor mutation runs on the runtime executor. Engine callbacks marshal onto
it before touching cursor state; there is no cross-thread reconciliation.

## Prepared next (gapless)

A prepared successor is correlated by an opaque engine commitment token, not
by TrackId or projection revision:

```text
PreparedCommitment {
  token,                 // globally unique PlaybackService/engine identity
  trackId,               // playback identity
  anchorState,
  anchorIndex
}
```

`PlaybackService::prepareNext()` returns the token, and the now-playing
transition event carries that token when it came from prepared-next. Explicit
plays carry no prepared token. The runtime may implement the token with the
engine's playback-item ID, but that audio-layer type does not leak through the
public cursor API. Tokens are unique across playback-session replacement, not
only within one projection revision.

Each commitment maintains its own anchor using the same range transforms and
post-batch identity reconciliation as the current track:

- While its `trackId` remains a member, it is Bound and refreshes from
  `indexOf(trackId)`.
- Once it leaves, it is Gap at the transformed old position.

This distinction matters when a track is pushed right by an insertion and is
removed later; treating every hint as a Gap would retain the wrong old
position.

After a batch, sequential playback re-resolves its successor. If the successor
TrackId differs from the active commitment, the runtime requests a replacement.
Shuffle uses the sticky-candidate rule below. Output-device, seek, or engine
changes may replace a commitment with the same TrackId but always receive a
new token.

Replacement has an acknowledgement contract:

- A token explicitly reported as disarmed by `clearPreparedNext()` is forgotten
  immediately.
- A replaced token that may already have latched in the engine becomes
  retired, remains anchor-maintained, and is kept until the next engine
  transition resolves which token won or an engine cancellation barrier proves
  that no such transition can still arrive.
- Once one token wins, all other retired tokens from that transition window
  are discarded. This bounds retention without guessing whether a queued
  transition callback exists.

When a prepared transition starts:

- Exact token match chooses the corresponding commitment. If the source is
  Live, the new current is Bound when the track is present and otherwise Gap at
  that commitment's maintained anchor. If the source is Invalidated, the
  current changes but anchor state remains non-authoritative.
- A transition with no prepared token is an explicit play. With a Live source
  it uses `indexOf()` when possible and otherwise the clamped previous current
  anchor; with an Invalidated source it updates only the current transport
  subject and leaves terminal cursor state untouched.
- A non-null token unknown to the active or retired set is stale, normally from
  a replaced session. `PlaybackService` rejects it before publishing
  now-playing; it is never reclassified as an explicit transition.
- The transition becomes the present and is never rolled back because
  membership changed.

Source invalidation asks the engine to disarm prepared-next and drops every
acknowledged token. An unacknowledged token remains recognizable. If its
already-started transition wins the race, that track becomes current and plays
out, while the cursor remains Invalidated and permits no further successor.

Prepared-next is therefore a **best-effort commitment, not a transaction**.
Coupling the audio engine transactionally to library writes is not justified
for this narrowly timed case.

## Shuffle

- Shuffle next remains memoryless random. Its pending candidate is sticky:
  unrelated membership changes do not re-roll it while its TrackId remains an
  eligible member. It is re-rolled when the candidate leaves, the relevant
  repeat/shuffle mode changes, or preparation fails.
- With repeat-one already handled by higher precedence, shuffle eligibility is
  every projected member except the current track when it is Bound. A Gap
  current excludes nothing. If no alternative exists, repeat-all may replay
  the sole projected current track; otherwise shuffle has no successor.
- The bounded history records the actual navigation path whether shuffle is on
  or off. Every successful transition to a different TrackId pushes the track
  being left, including sequential previous; restart and same-track replay do
  not. A transition initiated by popping shuffle history also does not push,
  so repeated previous commands continue walking backward instead of bouncing
  between two tracks.
- Shuffle `previous()` pops history. Entries outside current membership are
  discarded, as is a defensive entry equal to the current TrackId. An entry
  that fails to start is discarded and the pop continues, subject to the
  ordinary consecutive-failure bound. `hasPrevious()` scans for a valid
  history candidate without mutating the stack, after applying the
  elapsed-time restart rule. Repeat-all does not synthesize shuffle history.
- The stack is capped at 64 entries, cleared on source invalidation, and not
  persisted.
- The current anchor remains maintained while shuffle is on, so turning it off
  resumes sequential succession from the correct position.

No-repeat-until-cycle-complete is future policy and remains out of scope.

## Manual lists and List Order

Manual and smart lists unify below the cursor: `ManualListSource` and
`SmartListSource` both implement `TrackSource`, and the cursor sees only a
projection. Manual edits and smart re-evaluation use the same delta and anchor
rules.

A **List Order** builtin presentation (`groupBy = None`, empty `sortBy`)
exposes source order. With no comparator, the projection preserves source
positions. For a manual list this is the stored user arrangement:

- `recommendPresentation()` defaults manual lists to List Order.
- Reorder and insert-position edits are meaningful only under List Order. The
  UI-model policy exposes that gate; frontend editing controls remain deferred.
- Playback launched from a sorted manual-list view follows that sorted order;
  what the listener sees remains what the listener plays.
- A smart list may use List Order. It exposes the deterministic order inherited
  from its upstream source, but that order is not necessarily user-meaningful.

Every filtering source, including the ad-hoc quick-filter layer, exposes its
matches as a stable subsequence of upstream source order. An ID-sorted
membership container would violate this contract. Consequently a
quick-filtered manual list retains the relative user order of its remaining
tracks.

Manual lists are ordered sets, not multisets (`LibraryWriter` skips duplicate
adds and projection identity is `TrackId`). A future "play next" gesture for a
track already in the list is therefore a move, not a duplicate entry. An
app-managed manual list with a reserved role could provide ad-hoc queueing in
the future; that product feature is out of scope.

## Self-feedback lists

No library field is currently written by playback; smart-list membership
changes only through user edits and imports. If playback statistics such as
play count or last-played time are added later, the cursor needs no new
mechanism. These policies apply:

1. Prepared-next remains best effort.
2. Statistic writes are immediate and honest. The point at which a play counts
   is a domain decision; writes are never deferred to protect cursor order.
3. A query that drains itself is query semantics, not a defect. "Not played
   today" may empty as listening progresses and then stop; repeat-one may keep
   replaying the already-current track. Repeat-all over a least-played-first
   sort may cycle forever. No damping or special case is added.
4. Repeat-one ignores membership while the source remains Live.

## Session persistence

The composite playback-session payload contains:

- source list ID;
- `TrackOrderSpec` and quick-filter expression;
- `currentTrackId` and `anchorIndex`;
- playback position;
- shuffle, repeat, volume, and mute intent.

Prepared commitments, source validity, and shuffle history are transient.
This cursor-context payload is schema version 3; unsupported versions are
rejected rather than guessed.

Stopping removes the active cursor, but before removal the runtime captures an
immutable last-restorable cursor snapshot containing the launch context,
current track, and anchor. The composite save path pairs that snapshot with
`PlaybackService`'s last restorable position/state, preserving the existing
save-on-stop behavior; while a cursor is active, save reads that live cursor
instead. A later successful launch replaces the snapshot. Only an explicit
"forget playback session" operation clears it without replacement; ordinary
stop, terminal exhaustion, and source invalidation do not.

Cursor and playback snapshots are captured on the runtime executor. The
composite save rejects an internal current-TrackId mismatch rather than writing
a payload assembled from different playback generations.

Restore validates the context before constructing a cursor. Structurally
invalid IDs and malformed order/filter state reject the session; missing
library entities follow the explicit matrix below. Restore never autoplays; it
publishes a resolvable idle current target.

Source resolution is explicit:

- If the saved list still exists, or is virtual All Tracks, restore the saved
  order and quick filter.
- If the saved list no longer exists, fallback to All Tracks, preserve the
  saved `sortBy`, and clear the saved quick filter. The old anchor has no
  meaning in the substituted membership. Fallback succeeds only when the saved
  current track still exists in the library, and the substituted context is
  persistence-dirty.

After evaluating an existing source context, define
`restoreAnchor = min(savedAnchorIndex, size)`:

| Restore finding | Behavior |
| --- | --- |
| Current track exists in the library and projection | Restore it Bound at its current index and retain the saved position. |
| Current track exists in the library but not the projection | Restore it in Gap at `restoreAnchor` and retain the saved position. Membership controls only what follows. |
| Current track no longer exists, and `restoreAnchor < size` | Promote the row at `restoreAnchor` to the new idle current, enter Bound, reset position to zero, and mark the replacement persistence-dirty. |
| Current track no longer exists, gap is at the end, repeat-all is on, and the projection is non-empty | Promote row 0, enter Bound, reset position to zero, and mark dirty. |
| Current track no longer exists and no deterministic successor exists | Discard the session. |

Shuffle does not randomize missing-current replacement during restore. The
deterministic gap successor is selected first; restored shuffle/repeat intent
applies after that replacement becomes current. Repeat-one cannot rescue a
missing track, but if a replacement is chosen it applies to that new current.

For a missing-source fallback, an existing current track is necessarily Bound
in unfiltered All Tracks and retains its saved position. If both source and
current are missing, the session is discarded rather than applying an
unrelated old anchor to All Tracks.

After any successful restore, the runtime compares the normalized context,
current track, anchor, position, and modes with the loaded payload. Clamping,
fallback, replacement, or normalization that changes serialized state leaves
the restored session persistence-dirty; an unchanged restore starts clean.

Frontends retain only lifecycle timing: startup restore, dirty-event debounce,
significant-event and periodic saves, and shutdown save. Runtime code owns
validation, fallback, payload construction, and atomic restoration.

## Performance note

The session projection is always ungrouped and receives only the captured
`sortBy`, because group sections and display fields do not affect flattened
order. It still duplicates an open view's order index and sort keys. Benchmark
launch and delta cost on large libraries; if duplication matters, extract a
lightweight live order projection behind the same delta contract. That is an
optimization, not a playback-policy change.

The 10k-track projection regression constructs this detached form directly,
applies representative insert, remove, update, and reset batches, and
compile-time checks that `PlaybackSequenceState` exposes no materialized track
vector. The performance baseline measures projection construction, sorting, and
filter evaluation separately; the current implementation retains one session
projection and no second succession snapshot.

## Testing

The anchor state machine is suited to model-based tests using deterministic
runtime helpers. Generate valid insert/remove/update/move/reset batches against
a reference sequence and assert:

- while the source is Live, `anchorIndex` stays in `[0, size]` and Bound holds
  exactly when the current TrackId is a member;
- successor resolution matches the reference order under repeat modes;
- semantic revision changes exactly with its resolved tuple;
- persistence revision changes for every discrete serialized-intent mutation;
- no state or prepared commitment is observable mid-batch;
- source invalidation is terminal and no projection batch follows it.

Focused unit and integration tests additionally cover:

- strict view launch, virtual All Tracks, start-track validation, and
  prepare/commit rollback;
- lease pinning, parent-to-child invalidation, and quick-filter propagation;
- filtered manual-list source order;
- empty membership versus invalidated-source repeat behavior;
- shuffle history push/pop and failure walking;
- missing-current and missing-source restore cases;
- active, disarmed, and retired prepared-token races;
- dirty notification and save-revision acknowledgement while paused.

Changes to this architecture receive the normal full `./ao check` validation.
