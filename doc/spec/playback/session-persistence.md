---
id: playback.session-persistence
type: spec
status: current
domain: playback
summary: Defines restorable playback-state capture, validation, restoration, best-effort saving, discard, terminal library-switch retirement, and shutdown.
---
# Playback session persistence

## Scope

This specification defines current behavior for capturing, validating, restoring, normalizing, saving on natural application events, discarding, terminally retiring for a library switch, and shutting down the last restorable playback state.
The [playback session state reference](../../reference/playback/session-state.md) owns the exact version 4 payload.

It does not persist a materialized queue, workspace selection, prepared-next token, Engine generation, or decoder/output state.

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../architecture/system-overview.md), under the [playback](../../architecture/playback.md), [persistence](../../architecture/persistence-and-managed-state.md), and [interactive session lifecycle](../../architecture/interactive-session-lifecycle.md) architectures.
`PlaybackSessionPersistence` coordinates internal `PlaybackSuccession` and `PlaybackTransport`, the public `PlaybackService` snapshot, runtime library reads, the explicit `PlaybackSessionYamlSchema`, `ConfigStore`, and the async runtime.

## Terminology

- **Restorable state**: the serialized succession context, current subject, offset, modes, volume, and mute.
- **Deferred transport**: a restored idle current subject consumed by a later Play or PlayPause.
- **Normalization**: a valid restore whose live result differs from stored state because of fallback, replacement, or clamp.
- **Write seal**: the `WriteSealed` lifecycle, a permanent no-I/O rejection of observation start and every automatic, explicit, or shutdown save; it does not itself remove a group.
- **Terminal retirement**: successful group removal followed by the `Retired` lifecycle and its permanent save/rearm seal for the remaining process lifetime.

## Invariants

- Restore validates the complete version 4 payload before resolving a source.
- A candidate is prepared completely before it replaces live sequence or transport state.
- Restore never autoplays, arms an output route, or creates a frontend view.
- A restore failure before volume/mute application leaves prior public playback and restorable state unchanged.
- Volume and mute restore is sequential best effort: it returns the first typed backend failure and publishes the actual property state reached before that failure.
- Cursor and transport snapshots must name the same current track before save.
- A save captures one coherent cursor and transport value synchronously on the callback executor.
- A failed save leaves live playback state unchanged.
- Explicit discard and terminal retirement forget the last-restorable snapshots and stored group.
- Discard does not stop active playback.
- A write seal cannot be reversed by observation start, active-session mutation, queued snapshot publication, explicit checkpoint, or shutdown.
- Terminal retirement cannot be reversed by a later active-session mutation, queued snapshot publication, explicit checkpoint, or shutdown.

## State model

The owner retains one lifecycle enum, an orthogonal restore-in-progress guard, a
discarded-payload guard, the last observed `PlaybackService` snapshot,
subscriptions to succession restorable-state changes and committed snapshots,
and one scheduled debounce task.

| Lifecycle | Observation and save admission | Exit behavior |
|---|---|---|
| `Dormant` | No subscriptions or natural saves. Start, restore, checkpoint, or discard may establish the observation baseline. | Ordinary shutdown moves to `Shutdown` without saving; successful retirement moves to `Retired`. |
| `Observing` | Subscriptions and natural, explicit, and lifecycle checkpoints are admitted. | Ordinary shutdown performs the one final save and moves to `Shutdown`; write sealing moves to `WriteSealed`; successful retirement moves to `Retired`. |
| `WriteSealed` | Subscriptions and debounce are absent; start, natural triggers, explicit checkpoint, and shutdown perform no store write. | Retirement may still retry physical group removal and move to `Retired`; ordinary shutdown moves to `Shutdown` without saving. |
| `Retired` | The group was removed successfully, runtime restorable snapshots were discarded, and no save can be rearmed. | Repeated retirement and shutdown are no-ops. |
| `Shutdown` | Subscriptions and debounce are absent and no later save is admitted. | Repeated shutdown is a no-op; retirement returns `InvalidState`. |

Only `Dormant` moves to `Observing` when observation is ensured; calls made in a
sealed or terminal lifecycle do not rearm subscriptions. Restore-in-progress
suppresses checkpoints independently of that enum. The discarded-payload guard
suppresses saves after ordinary discard until a qualifying active-session
mutation admits a new payload; it is not a terminal lifecycle.

The succession and transport services each retain a last-restorable snapshot after ordinary stop, exhaustion, or invalidation removes live state.
A later successful launch replaces those snapshots.

The save debounce is one second.

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
| Current is missing and anchor has a successor. | Promote successor, bind, and reset offset. |
| Current is missing at end, repeat-all is on, and projection is non-empty. | Promote row zero and reset offset. |
| Current is missing with no deterministic successor. | Discard candidate. |

Shuffle does not select a replacement during restore.
Deterministic recovery establishes current before restored shuffle and repeat policy become active.

When the source list is missing, restore substitutes All Tracks, retains sort terms, and clears quick filter.
Fallback succeeds only when the saved current track still exists and retains offset.
A missing source plus missing current discards the candidate.
When the source exists but its own or an ancestor's stored filter is invalid, restore returns the contextual `FormatRejected` error and installs no cursor candidate.

Offset is clamped against resolved duration.
An offset at or beyond end becomes zero.
Fallback, replacement, anchor clamp, offset clamp, or any other normalization changes only the in-memory restored candidate.
Restore does not immediately write the normalized value back; the next natural or lifecycle checkpoint captures it.

### Prepare and install

The owner constructs the same lease, filter, and detached projection chain used by a view launch without creating a view.
It prepares candidate cursor, idle current target, modes, position, volume, and mute.
One `PlaybackService` restore commit applies volume and then mute, installs deferred transport without lower publication only after both properties succeed, then installs the prepared succession session and finally publishes their combined snapshot.
If either property fails, the restore returns that typed error and publishes the actual volume/mute state reached at the backend; an earlier successful property write remains applied.
It does not attempt rollback through the same fallible backend API.
There is no callback from transport into cursor installation and no intermediate transport-only or cursor-only public state.
A restore that installs a candidate always publishes a new position anchor, even
when the subject is unchanged. The persistence owner consumes that exact
snapshot as the restored observation baseline, so a repeated idle restore does
not look like a later state change and a changed offset is immediately
observable.

The first later Play or PlayPause consumes the deferred token and starts the resolved subject at the offset.
GTK may navigate/reveal after success, but workspace selection is not part of this transaction.

### Capture and save triggers

While active, save captures live launch context, current and anchor.
After ordinary clear/stop/exhaustion/invalidation, it uses immutable last-restorable cursor and transport snapshots.
A mismatch rejects save.

Launch spec, current/anchor, modes, volume/mute, and final seek are restorable state.
Elapsed progress, projection churn with unchanged anchor, source invalidation alone, prepared-token changes, sticky shuffle candidate changes, and shuffle history are not save triggers.
Elapsed position is sampled when save is requested.

Succession restorable-state changes and volume/mute changes request a one-second trailing debounce.
Subject changes, final seeks, and transitions to paused or idle request an immediate checkpoint because they already provide a natural application boundary.

### Schedule, save, and checkpoint

From `Dormant`, an explicit observation start, restore, checkpoint, or discard establishes the observation baseline, connects state subscriptions, and admits debounce work.
No other lifecycle can transition back to `Observing`.
GTK and WinUI start observation when they activate an ordinary restoring
session. A desktop successor idle session reads no stored payload and starts
observation only after its selected root is durable.
An explicit checkpoint writes only from `Dormant` or `Observing`; ordinary
shutdown applies the lifecycle-specific final-save policy below.

Save writes the payload through an explicit `PlaybackSessionYamlSchema` and one result-bearing `ConfigStore::save` candidate commit.
There is no playback-specific dirty bit, durable acknowledgement, or retry scheduler.
An automatic save failure is logged and waits for the next natural trigger; an explicit checkpoint returns the typed failure to its caller.

Ordinary elapsed progress does not schedule a checkpoint.
An abrupt process termination during uninterrupted playback may therefore restore the position captured by the last successful checkpoint, with no bounded position-freshness guarantee.
Frontends request restore/checkpoint and shut down the owner but do not implement save scheduling policy.

### Discard

Discard atomically removes the `playback-session` group through `ConfigStore`.
Only after removal succeeds does it clear succession/transport restorable snapshots and enter discarded state.
Explicit and lifecycle checkpoints remain no-ops while discarded until a later discrete active-session mutation admits future saves again.

### Seal writes without removal

The no-I/O write seal moves `Dormant` or `Observing` to `WriteSealed`,
disconnects observation, cancels debounce,
and makes observation start, natural triggers, explicit checkpoints, and
shutdown saves no-ops. It does not mutate the store or clear the runtime's
last-restorable snapshots.

GTK and WinUI use this seal when a successor's selected-root commit fails after
the parent has already removed the global payload. The active successor remains
usable, but it cannot persist resume intent under the prior durable root.
A later terminal retirement may still attempt physical group removal and clear
the retained runtime snapshots.

### Retire for a library switch

Terminal retirement is idempotent in `Retired` and fails with `InvalidState` while restore is in progress or after ordinary shutdown has reached `Shutdown`.
It atomically removes the same `playback-session` group as ordinary discard.
Removal failure changes no terminal state, so the caller may retain the current library and retry.

Only after removal succeeds does retirement enter `Retired`, disconnect succession and snapshot subscriptions, cancel the debounce task, and clear succession and transport restorable snapshots.
`Retired` shares shutdown's save-admission barrier but is distinguished so a repeated retirement remains a successful no-op.
Already queued playback commands may still change live playback before runtime teardown, but their publications have no persistence subscriber and every explicit checkpoint is a no-op.
Later ordinary shutdown is idempotent and cannot perform a final save.

## Failure and cancellation

Malformed structural deserialize, unsupported versions, schema semantic validation, source/filter/projection construction, transport preparation, and store failures return typed results.
Structural and transport-preparation failures are fail-closed with respect to live/restorable state.
An invalid retained source filter is one such source-construction failure; the frontend may log it and leave default playback active, but the runtime does not reinterpret it as a successful empty session.
Volume/mute property failure follows the sequential best-effort contract above; the stored payload remains unchanged, while the public snapshot reflects any property write that succeeded.
Discard and terminal retirement remain fail-closed with respect to physical group removal.
The no-I/O write seal has no store failure path and does not claim physical removal.

Scheduled debounce uses one cancellable shared-runtime task and a cancellation-checked callback-executor hop; replacement or checkpoint retires that task before admitting another.
Automatic failures do not create background retry work.
Ordinary shutdown cancels pending debounce and enters `Shutdown`; it performs its final checkpoint while borrowed services and store still exist only when leaving `Observing`.
Terminal retirement instead cancels and enters `Retired` without a final save.

## Persistence and versioning

The literal group is `playback-session`.
Only schema version `4` is accepted; older or newer values are rejected rather than migrated.

GTK injects its global application config as the playback-session store. WinUI
injects its separate global `windows-playback.yaml` store. Current TUI
composition uses its runtime workspace config when no separate store is
injected.
The payload itself contains library-scoped track/list ids but no durable library identity.
Both desktop switch lifecycles checkpoint the active graph, physically remove
this group, and terminally seal playback persistence in the parent before that
graph is destroyed.
Only after complete parent teardown does a successor activate the explicit target with idle playback.
Successful selected-root persistence admits future playback writes; failure keeps the prior root, no payload, and the permanent write seal, so no process interprets one library's ids against another root.

## Frontend observations

Restore returns whether a session was restored plus current track and source identities.
It never starts audio.
GTK may use a successful restore to reveal the actual current track. WinUI
restores before its controllers bind and then projects the restored snapshot
through its ordinary playback command and presentation adapters.
TUI currently does not run the same startup/checkpoint sequence; that asymmetry belongs to interactive lifecycle architecture.

## Implementation map

- [`PlaybackSessionPersistence.h`](../../../app/runtime/PlaybackSessionPersistence.h) and [`PlaybackSessionPersistence.cpp`](../../../app/runtime/PlaybackSessionPersistence.cpp) own behavior.
- [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h) owns payload and internal transport snapshot values.
- [`PlaybackSessionYamlSchema.h`](../../../app/runtime/PlaybackSessionYamlSchema.h) and [`PlaybackSessionYamlSchema.cpp`](../../../app/runtime/PlaybackSessionYamlSchema.cpp) own explicit YAML mapping, version dispatch, and pre-restore validation.
- [`AppRuntime.cpp`](../../../app/runtime/AppRuntime.cpp) owns public composition and lifecycle forwarding.

## Test map

- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp) protects payload validation, restore matrix, coherent and same-subject restore publication, observation-only natural saves, sequential volume/mute failure, deferred observer commands, event-driven timing, failed-save recovery on a later change, ordinary discard, terminal retirement against queued/debounced/expired-callback/explicit save paths, store selection, and structural failure atomicity.
- [`MainWindowTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowTest.cpp) protects the successor root-commit gate and no-I/O playback-write seal against natural, explicit, window, and shutdown playback saves while ordinary window, output, layout, and workspace saves continue over the shared GTK store.
- [`SelectedRootCommitTest.cpp`](../../../test/unit/winui/app/SelectedRootCommitTest.cpp)
  and [`DestructiveLibraryRestartTest.cpp`](../../../test/unit/winui/app/DestructiveLibraryRestartTest.cpp)
  protect WinUI's immutable root candidate and preparation-failure boundary;
  native WinUI builds protect the runtime store injection and restore call path.
- [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) protects frontend-neutral restoration primitives.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [Playback session state reference](../../reference/playback/session-state.md)
- [Playback succession cursor](cursor.md)
- [Grouped configuration store](../persistence/config-store.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
