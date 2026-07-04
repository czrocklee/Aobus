# Listening Session Resume

Aobus persists the last listening context as workspace configuration under the
`playback-session` group. The restored session is a deferred playback token, not
an armed audio pipeline.

## Persisted State

The session records:

- schema version,
- source list id,
- current track id,
- position in milliseconds,
- shuffle and repeat modes,
- volume and mute intent.

The source list is the durable queue source. Smart lists are re-evaluated on
restore, so membership drift is expected behavior. If the saved track is no
longer present in the restored source, playback falls back to the source head at
position 0. If the saved track is present in the source but no longer resolves
in the library, restore uses the same source-head fallback. If the source list
no longer exists, restore falls back to All Tracks.

Malformed persisted values are normalized at the restore boundary: unsupported
shuffle/repeat enum values become Off, non-finite volume becomes 1.0, volume is
clamped to 0.0-1.0, and oversized positions are clamped before duration
handling. Unsupported schema versions are rejected instead of guessed.

When a saved position is at or past the track duration, the next persisted
position is 0. This avoids restarting directly at end-of-stream after a natural
track end.

## Deferred Restore

Startup restore must not autoplay and must not open an audio route. The GTK
coordinator reloads track sources, rebuilds the queue from the current source
membership, and asks `PlaybackQueueModel`/`PlaybackService` to publish an idle
now-playing state. `PlaybackService` keeps the resolved playback request and
position as a deferred resume token.

Selection is not persisted as part of the playback session. It remains
workspace/view UI state. After a playback session is restored, the GTK
coordinator focuses the restored source view and selects the actual restored
track so the visible workspace matches the idle now-playing state.

The transport Play/PlayPause action treats an idle now-playing track as a
resume target. That lets restored sessions consume the deferred resume token and
start from the saved position instead of replaying the selected track from the
beginning.

The first explicit play/resume action consumes that token and starts playback
with the saved initial offset. The audio engine seeks the source before starting
the backend stream, so the first audible samples come from the restored
position.

The GTK coordinator saves the session on final seek, stop, track change, and
shutdown, plus a periodic autosave while playback is Playing. The periodic path
is intentionally gated by transport state so an idle restored session does not
touch the audio route or churn the workspace file.

## Shuffle Continuity

Shuffle mode is restored, but shuffle order is not. The queue's current shuffle
implementation is memoryless; there is no seed or history to persist. True
shuffle continuity requires a separate shuffle-history model.
