---
id: playback.session-persistence
type: spec
status: current
domain: playback
summary: Defines listening-intent capture, strict validation, candidate restoration, fallback, deferred transport, dirty revisions, saving, retry, discard, and shutdown.
---
# Playback session persistence

## Scope

This specification defines current behavior for capturing, validating, restoring, normalizing, saving, retrying, discarding, and shutting down the last restorable listening intent.
The [playback session state reference](../../reference/playback/session-state.md) owns the exact version 3 payload.

It does not persist a materialized queue, workspace selection, prepared-next token, Engine generation, or decoder/output state.

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../architecture/system-overview.md), under the [playback](../../architecture/playback.md), [persistence](../../architecture/persistence-and-managed-state.md), and [interactive session lifecycle](../../architecture/interactive-session-lifecycle.md) architectures.
`PlaybackSessionPersistence` coordinates internal `PlaybackSuccession` and `PlaybackTransport`, the public `PlaybackService` snapshot, runtime library reads, the explicit `PlaybackSessionYamlSchema`, `ConfigStore`, and the async runtime.

## Terminology

- **Restorable intent**: the serialized succession context, current subject, offset, modes, volume, and mute.
- **Deferred transport**: a restored idle current subject consumed by a later Play or PlayPause.
- **Normalization**: a valid restore whose live result differs from serialized intent because of fallback, replacement, or clamp.
- **Composite revision**: the monotonic revision covering every serialized-intent owner.
- **Captured revision**: the exact revision associated with one save candidate.

## Invariants

- Restore validates the complete version 3 payload before resolving a source.
- A candidate is prepared completely before it replaces live sequence or transport state.
- Restore never autoplays, arms an output route, or creates a frontend view.
- A failed restore leaves prior cursor, transport, modes, volume/mute, and revisions unchanged.
- Cursor and transport snapshots must name the same current track before save.
- A successful save acknowledges only the captured revision.
- A newer mutation remains dirty after an older save completes.
- Only explicit discard forgets the last-restorable snapshots and stored group.
- Discard does not stop active playback.

## State model

The owner retains started/shutdown/restoring/discarded flags, current volume/mute/position intent, one composite revision, subscriptions to succession persistence intent and committed `PlaybackService` snapshots, one scheduled debounce-or-retry task, one periodic task, retry delay, and schedule generation.

The succession and transport services each retain a last-restorable snapshot after ordinary stop, exhaustion, or invalidation removes live state.
A later successful launch replaces those snapshots.

Scheduled save state is `None`, `DirtyDebounce`, or `Retry`.
Retry delay starts at one second and doubles to a maximum of one minute.
Periodic sampling runs every ten seconds; ordinary dirty debounce is one second.

## Commands and transitions

### Validate

Restore rejects a schema mismatch, invalid ids, overflowing anchor/position, unsupported shuffle/repeat/volume, invalid or duplicate sort fields, excess sort terms, and a filter that cannot parse and compile.
This occurs before source lookup, so source absence cannot hide malformed serialized state.

### Resolve current and source

For an existing source, the saved anchor is clamped to live projection size.

| Finding | Restore result |
|---|---|
| Current exists and is projected. | Bound at its current live index; retain offset. |
| Current exists but is filtered out. | Gap at restored anchor; retain offset. |
| Current is missing and anchor has a successor. | Promote successor, bind, reset offset, mark dirty. |
| Current is missing at end, repeat-all is on, and projection is non-empty. | Promote row zero, reset offset, mark dirty. |
| Current is missing with no deterministic successor. | Discard candidate. |

Shuffle does not select a replacement during restore.
Deterministic recovery establishes current before restored shuffle and repeat policy become active.

When the source list is missing, restore substitutes All Tracks, retains sort terms, and clears quick filter.
Fallback succeeds only when the saved current track still exists; it retains offset and starts dirty.
A missing source plus missing current discards the candidate.

Offset is clamped against resolved duration.
An offset at or beyond end becomes zero.
Fallback, replacement, anchor clamp, offset clamp, or any other changed serialized intent marks the restored session dirty; an exact restore is clean.

### Prepare and install

The owner constructs the same lease, filter, and detached projection chain used by a view launch without creating a view.
It prepares candidate cursor, idle current target, modes, position, volume, and mute.
One `PlaybackService` restore commit first installs deferred transport without lower publication, then installs the prepared succession session, and finally publishes their combined snapshot.
There is no callback from transport into cursor installation and no intermediate transport-only or cursor-only public state.
A restore that installs a candidate always publishes a new position anchor, even
when the subject is unchanged. The persistence owner consumes that exact
snapshot as the restored clean baseline, so a repeated idle restore cannot make
the next unrelated mutation appear clean and a changed offset is immediately
observable.

The first later Play or PlayPause consumes the deferred token and starts the resolved subject at the offset.
GTK may navigate/reveal after success, but workspace selection is not part of this transaction.

### Capture and dirty revision

While active, save captures live launch context, current and anchor.
After ordinary clear/stop/exhaustion/invalidation, it uses immutable last-restorable cursor and transport snapshots.
A mismatch rejects save.

The composite revision advances for launch spec, current/anchor, modes, volume/mute, and final seek.
It does not advance for elapsed progress, projection churn with unchanged anchor, source invalidation alone, prepared-token changes, sticky shuffle candidate changes, or shuffle history.
Elapsed position is sampled when save is requested.

Only clean-to-dirty emits `onDirty`.
A subscriber added while dirty receives immediate replay.
Observer exceptions are logged and do not stop other persistence behavior.

### Schedule, save, retry, and checkpoint

`start()` connects intent subscriptions, starts periodic sampling, and admits debounce/retry work.
Ordinary dirty state schedules a one-second debounce.
Significant events request immediate checkpoint through the same save state machine.

Save writes the payload through an explicit `PlaybackSessionYamlSchema` and one result-bearing `ConfigStore::save` candidate commit.
Only complete success acknowledges the captured revision and resets retry delay.
Load, serialize, emission, or replacement failure keeps dirty and schedules bounded exponential retry even while paused.

Periodic and shutdown checkpoints are safety nets, not the only route to durability.
Frontends start, restore/checkpoint, and shut down the owner but do not implement the timer policy.

### Discard

Discard atomically removes the `playback-session` group through `ConfigStore`.
Only after removal succeeds does it clear succession/transport restorable snapshots and enter discarded state.
Periodic saves remain no-ops while discarded until a later discrete active-session mutation establishes new dirty intent.

## Failure and cancellation

Malformed structural deserialize, unsupported versions, schema semantic validation, source/filter/projection construction, transport preparation, and store failures return typed results.
Restore and discard are fail-closed with respect to live/restorable state as described above.

Scheduled tasks use the shared async runtime and owner lifetime.
Shutdown cancels pending debounce/retry and periodic work, then performs its final checkpoint while borrowed services and store still exist.
An already committed newer intent is never reinterpreted as saved by cancellation or an older completion.

## Persistence and versioning

The literal group is `playback-session`.
Only schema version `3` is accepted; older or newer values are rejected rather than migrated.

GTK injects the global application config as the playback-session store, while current TUI composition uses its runtime workspace config when no separate store is injected.
The payload itself contains library-scoped track/list ids but no durable library identity.
The current GTK switch workflow discards it before replacing the active library; RFC 0019 proposes library-bound sessions and transactional switching.

## Frontend observations

Restore returns whether a session was restored plus current track and source identities.
It never starts audio.
Dirty publication is a persistence signal, not a user-facing progress message.

GTK may use a successful restore to reveal the actual current track.
TUI currently does not run the same startup/checkpoint sequence; that asymmetry belongs to interactive lifecycle architecture.

## Implementation map

- [`PlaybackSessionPersistence.h`](../../../app/runtime/PlaybackSessionPersistence.h) and [`PlaybackSessionPersistence.cpp`](../../../app/runtime/PlaybackSessionPersistence.cpp) own behavior.
- [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h) owns payload and internal transport snapshot values.
- [`PlaybackSessionYamlSchema.h`](../../../app/runtime/PlaybackSessionYamlSchema.h) and [`PlaybackSessionYamlSchema.cpp`](../../../app/runtime/PlaybackSessionYamlSchema.cpp) own explicit YAML mapping, version dispatch, and pre-restore validation.
- [`PlaybackSessionRevision.h`](../../../app/runtime/playback/PlaybackSessionRevision.h) owns exact dirty acknowledgement.
- [`AppRuntime.cpp`](../../../app/runtime/AppRuntime.cpp) owns public composition and lifecycle forwarding.

## Test map

- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp) protects payload validation, restore matrix, coherent and same-subject restore publication, repeated-restore baselines, deferred observer commands, deferred start, dirty events, timing, retry, discard, store selection, and failure atomicity.
- [`PlaybackSessionRevisionTest.cpp`](../../../test/unit/runtime/playback/PlaybackSessionRevisionTest.cpp) protects captured-revision acknowledgement.
- [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) protects frontend-neutral restoration primitives.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [Playback session state reference](../../reference/playback/session-state.md)
- [Playback succession cursor](cursor.md)
- [Grouped configuration store](../persistence/config-store.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
