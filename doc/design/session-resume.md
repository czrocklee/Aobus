# Listening Session Resume

Aobus persists the last restorable listening intent as workspace configuration
under the `playback-session` group. Restore reconstructs a live playback cursor
and an idle transport token; it never autoplays or arms an output route.

## Schema v3

The current payload records:

- schema version 3;
- source list ID, quick-filter expression, and ordered sort terms;
- current track ID and cursor anchor index;
- position in milliseconds;
- shuffle and repeat modes;
- volume and mute intent.

Prepared-next tokens, source validity, shuffle history, projection rows, and
audio-engine generations are transient. In particular, the payload contains no
materialized track vector or queue index. Older schema versions are rejected
rather than migrated or guessed.

Restore validates the complete serialized context before resolving a source.
Invalid IDs, overflowing anchor or position values, unsupported mode or volume
values, invalid or duplicate sort fields, excessive sort terms, and a filter
that cannot be parsed and compiled all reject the payload. This ordering is
important: a missing source cannot turn a malformed filter or order into a
successful All Tracks fallback.

## Restore Matrix

For an existing source, `restoreAnchor` is the saved anchor clamped to the live
projection size.

| Finding | Result |
| --- | --- |
| Current track exists and is projected | Restore it Bound at its current live index and retain position. |
| Current track exists but is filtered out | Restore it in Gap at `restoreAnchor` and retain position. |
| Current track is missing and `restoreAnchor` has a successor | Promote that row, enter Bound, reset position to zero, and mark the normalized session dirty. |
| Current track is missing at the end, repeat-all is enabled, and the projection is non-empty | Promote row 0, reset position to zero, and mark dirty. |
| Current track is missing with no deterministic successor | Discard the saved session. |

Shuffle never chooses a replacement during restore. Deterministic gap recovery
happens first; restored shuffle and repeat policy apply only after a current
track has been established.

If the source list is missing, restore substitutes All Tracks, preserves the
sort terms, and clears the quick filter. Fallback succeeds only when the saved
current track still exists, in which case position is retained and the
substituted context starts dirty. A missing source plus a missing current track
discards the session.

Position is clamped against the resolved track duration. A position at or past
the end becomes zero so resume cannot start directly at end-of-stream. Any
fallback, replacement, anchor clamp, position clamp, or other normalization
that changes the serialized intent starts dirty; an exact restore starts clean.

## Atomic Deferred Restore

`PlaybackSessionPersistence` owns validation, source fallback, cursor
construction, transport preparation, and ConfigStore error handling. It builds
the same lease/filter/detached-projection chain used by a view launch, but does
not pretend that a view exists.

The candidate cursor, idle current target, modes, position, volume, and mute are
prepared before installation. Only after preparation succeeds does one executor
transaction replace the sequence and publish the deferred transport state. A
failure leaves the previous cursor, transport, modes, volume/mute, and revisions
unchanged.

The first later Play or PlayPause consumes the deferred transport token and
starts the resolved track at the restored offset. Selection remains workspace
UI state rather than playback-session state; after a successful restore the GTK
coordinator may navigate to the restored source and reveal the actual current
track.

## Active and Last-Restorable State

While a cursor is active, saves capture its live context, current track, and
anchor. Before clear, stop, terminal exhaustion, or source invalidation can
remove succession authority, the runtime keeps an immutable last-restorable
cursor snapshot. The transport owns the matching last-restorable current and
position snapshot. Saving rejects a cursor/transport current-track mismatch
instead of writing a payload assembled from different playback generations.

Ordinary stop, exhaustion, or invalidation does not forget listening intent. A
later successful launch replaces the snapshot. `discardRestorablePlaybackSession()` is the
only operation that removes the config group and clears both snapshots; it does
not stop active audio. Snapshot clearing occurs only after remove and flush
succeed. Periodic saves remain no-ops while forgotten until a later discrete
active-session mutation makes new intent dirty.

## Dirty Lifecycle and Save Timing

Persistence owns one monotonic composite revision. It advances once for each
discrete serialized-intent change: launch spec, current track or anchor,
modes, volume/mute, and final seek. Projection churn that leaves the anchor
unchanged, source invalidation alone, prepared-token replacement, sticky shuffle
candidate changes, and shuffle-history operations do not dirty the payload.
Elapsed playback progress is sampled when a save is requested; it does not
advance the revision on every clock tick.

Only clean-to-dirty publishes `onPlaybackSessionDirty()`. A successful save
acknowledges the exact captured revision; a newer mutation remains dirty. Load,
save, or flush failure retains dirty state. Subscribing while already dirty
immediately replays the dirty condition, which makes late frontend startup and
normalization during restore safe.

`PlaybackSessionPersistence` also owns save timing. It starts before restore,
debounces ordinary dirty events, performs immediate significant-event saves,
and retries failures with bounded exponential backoff even while paused.
Significant, periodic, and shutdown attempts use the same persistence state
machine; periodic and shutdown saves are safety nets, not the only way paused
intent becomes durable. Frontends only start and shut down this application
lifecycle.

GTK treats playback as one application session paired with the globally stored
last-open library. One process owns one active library runtime and one main
library window. Switching libraries first saves and forgets the old playback
session, destroys the old runtime, updates the global library path, and then
constructs the replacement runtime. The playback-session payload therefore
lives in the global application config, while view/layout workspace state stays
in the library-specific workspace config. This ordering prevents database-local
track and list IDs from crossing a library boundary.

## Shuffle Continuity

Shuffle mode is restored, but the transient sticky candidate and bounded
history are not. A restored cursor begins a new shuffle navigation history over
the restored live projection.
