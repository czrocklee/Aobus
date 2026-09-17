---
id: playback.application-commit
---
# Playback application commits

## Scope

This specification defines the application-level playback boundary: public
command ordering, coherent snapshot commits, observer reentrancy, queued-command
supersession, lower-layer settlement, and shutdown.
The [service](../../../app/include/ao/rt/playback/PlaybackService.h),
[commands](../../../app/include/ao/rt/playback/PlaybackCommands.h),
[events](../../../app/include/ao/rt/playback/PlaybackEvents.h), and
[snapshot](../../../app/include/ao/rt/playback/PlaybackSnapshot.h) headers own the current declarations.
This is an in-process API with no independent serialized, wire-protocol, or source-compatibility promise; its consumers evolve with runtime.
Persisted listening intent has the separate [session-state format](../../reference/playback/session-state.md).

Succession policy belongs to the [playback cursor](cursor.md), audio execution
and generation fences belong to [audio execution](audio-execution.md), and
durable restore policy belongs to [session persistence](session-persistence.md).

## Code boundary

This behavior belongs to `PlaybackService` in the **application runtime** layer
of the [system architecture](../overview.md), under the
[playback architecture](README.md). Public declarations
live in `app/include/ao/rt/playback/`; the private implementation in
`app/runtime/playback/PlaybackService.cpp` owns commit state and the serial
command queue while borrowing the runtime-internal `PlaybackTransport` and
`PlaybackSuccession` owners, the music library and its committed change stream.

## Terminology

- **Logical commit** is one callback-executor-affine transition from the last
  coherent snapshot to a newly composed snapshot.
- **External settlement** is accepted lower-layer state arriving from Player,
  Engine, a provider, or natural-advance callbacks rather than a public command.
- **Publication turn** is delivery of one snapshot or transient event to all
  currently connected observers.
- **Command generation** is the internal ordering value used to discard an old
  queued start or navigation command after a newer invalidating command.
- **Playback occurrence** is Transport's runtime-only identity for one installed
  current subject, independent from TrackId, audio item id, and persisted state.
- **Pending view start** is the private candidate succession session and request
  retained after synchronous validation while audio preparation runs on a worker.

## Invariants

- A logical commit publishes exactly one snapshot when semantic content changes
  and publishes nothing for a no-op.
- Snapshot access, command/event role access, subscription, command submission,
  and shutdown are confined to the runtime callback executor.
- A snapshot is assembled only after matching transport and succession changes
  have settled.
- Runtime projects backend volume into a finite application level in `[0, 1]`, clamping finite out-of-range values and using `1` for non-finite values, including while muted.
  This projection leaves raw audio-layer gain and device volume unchanged.
- While succession is active, transport and succession identify the same current
  track and source.
- A command issued by a playback observer never executes on that observer stack.
- Once the deferred queue is non-empty, later public commands join its tail; no
  synchronous command overtakes it.
- Stop, replacement start, next, previous, restore, output-route change, clear,
  and shutdown supersede older queued start/navigation commands. Orthogonal
  volume, mute, pause, resume, seek, shuffle, and repeat commands remain FIFO;
  occurrence-guarded seeks perform their own execution-time identity check.
- Elapsed clock drift alone is not semantic content and causes no publication.
  `elapsed()` samples nonnegative live position without publishing, bounded by the committed duration when positive.
  Runtime/audio occurrence checks before and after the sample keep it correlated to the last committed snapshot; a mismatch falls back to that snapshot's anchor.
- Every installed explicit start, replay, natural advance, and deferred restore
  receives a new nonzero playback occurrence; refresh, pause, resume, and seek
  preserve it, and Stop invalidates it.
  Natural terminal Idle without deferred resume ownership also invalidates it, while a natural successor keeps its newly installed identity.
- An unexpected command or settlement exception terminally closes the service, retains the last committed snapshot, and abandons the queued backlog.
- Shutdown closes command admission and makes deferred service tasks safe to
  drop before transport callback producers are quiesced.

## State model

The service retains the last committed snapshot, position and final-seek
counters, command generations, one FIFO, at most one scheduled drain, lower
subscriptions, pending seek previews and reveal requests, commit/publication
depth, and one weak deferred-task gate. Succession additionally retains at most
one pending view start and one pending lookahead successor identity.

This state is confined to the runtime callback executor. The command queue adds
no worker or mutex. Player owns the cancellable `async::Runtime` tasks used for
view-start and gapless-lookahead decoder/source preparation; workers carry
isolated audio values and do not own application state.

## Snapshot borrowing and output intent

A snapshot reference borrows the last coherent transport and succession state only until the next publication or service destruction.
Copy it before retaining an old value across any command or callback that may publish.
Semantic equality excludes the correlated elapsed sample: the clock anchor is the tuple of transport state, position revision, and duration, not the time of a metadata-only publication.

Runtime owns the selected `audio::OutputDeviceSelection` and available route catalog.
An empty device id denotes a default only when the provider advertises a compatible empty-id device; it is not a universal default route.
The neutral selection type has no persistence lifecycle of its own.
UIModel owns interactive restore admission, and frontend composition owns durable schema, submission, and checkpoint timing; see [presentation](../presentation/README.md).

## Live Now Playing metadata

`PlaybackService` resolves the current track's title, artist, album and primary
cover from one library read transaction when the playback request changes.
Read admission refreshes the append-only dictionary through that transaction's
visible tail before resolving artist and album ids.
Committed insertions, mutations and deletions intersecting that track, and a
library reset, invalidate the cached presentation. The next coherent snapshot
publication refreshes all four fields together, including while paused.
Unrelated track changes do not invalidate it; unchanged presentation publishes
nothing. Volume, quality and position updates reuse the cached metadata.

A prepared successor resolves current metadata when it becomes the active
request, even if it was edited after preparation. A missing track retains the
request's launch text and has no cover; an already-open audio source can keep
playing, including when the record was edited before deletion. Requests without
a library track id retain their supplied metadata and cover. A removed library
cover stays absent across later transport snapshots. An empty title does not
make a library track inactive in the Now Playing view model.

This presentation overlay leaves the transport's launch request, decoder input,
source provenance, position/final-seek revisions and succession recovery state
unchanged. Metadata invalidation uses the existing deferred snapshot boundary;
it does not emit a transport `NowPlayingChanged` position-anchor event. Commands
submitted by metadata observers obey the same reentrancy and shutdown rules as
other snapshot observers. An invalidation delivered reentrantly during command
settlement schedules another publication after the outer commit closes.
Synchronous service destruction or shutdown inside its command/publication
boundary is rejected by contract; owners must defer teardown until it unwinds.

## Commands and transitions

### Admission and execution

Outside a commit or publication, and with no queued backlog, a command executes
synchronously. During a commit or publication, or while a backlog exists, it is
appended to the FIFO. One deferred task consumes one command and schedules the
next drain after settlement.

`startFromView` reports synchronous view, membership, request, readiness, and
worker-task admission. Success does not mean that decoder preparation completed
or that a new current subject was installed. When called by an observer it
initially reports successful command-queue admission; it has no separate public
completion token.

The occurrence-bearing `seek` overload is a guarded queued positioning command. It keeps
normal FIFO ordering through an active commit, publication, or backlog, captures
occurrence, elapsed position, and mode by value, and validates occurrence and
range in Transport when the operation executes. A stale queued Preview or Final
issues no seek and publishes no seek update; a valid queued Preview therefore retains its matching Final
settlement instead of leaving frontend preview state behind merely because an
orthogonal command temporarily owned the boundary. The original occurrence-free
`seek` remains the unconditional compatibility command.

`seekBy` captures an occurrence and signed delta in the same FIFO.
At execution it requires the matching occurrence and positive duration, samples live Transport elapsed, and clamps the relative target without overflowing before issuing a guarded Final.
Its default `Clamp` end behavior preserves UI positioning; `Next` instead advances for a positive offset strictly past the live endpoint, only with an available successor and matching runtime/audio identity.
The accepted Next uses existing succession cancellation of pending explicit preparation and lookahead; a rejected or ordinary relative seek does not invalidate command generations.
An exact-end target remains a seek, and unavailable or stale past-end Next is a no-op.
Sequential queued relative commands therefore accumulate from each execution's live position rather than a cached publication; headless consumers do not need rendering subscriptions.
The live query and seek are separate observations: an intervening realtime replacement is rejected by the existing Engine item guard, not treated as a new target.

`trySeek` is a synchronous-only guarded final seek. It returns `false` without
queuing when admission is closed, a commit or publication is active, or the
command FIFO has a backlog. Otherwise it runs through normal commit settlement
without changing the command-invalidating generation and returns whether
Transport and Engine accepted the expected occurrence/audio-item chain.

`tryNext` is likewise synchronous-only. It first validates the nonzero runtime
occurrence and subject. Active playback then asks Player and Engine whether the
settled realtime timeline still exposes Transport's expected audio item; an
exact restored Idle occurrence is the sole no-active-audio exception. Only a
match advances the invalidating generation and enters normal synchronous Next
planning. The Engine observation releases its control lock before succession
runs. This admission observation does not replace `trySeek`'s mutation-time
identity guards.

Session restore keeps its call-level result on `AppRuntime` and is rejected
while a commit, publication, or backlog is active.

### Supersession

A queued start, next, or previous command records its generation. A newer
replacement start, navigation, restore, stop, clear, output-route change, or
shutdown raises the invalidating generation. An older generation-fenced start
or navigation command is discarded before touching either lower owner when it
reaches the queue head. A guarded seek remains FIFO and lets Transport reject
its captured occurrence if replacement has already executed.

### Explicit start and navigation

Succession synchronously validates and constructs a cursor candidate, then
admits isolated audio preparation without replacing public state. An inspection decoder
opens on an async worker and records logical signal facts.
Preparation briefly returns to the callback executor so Engine can revalidate its captured route and obtain the selected Backend's non-blocking prewarm hint, then resumes on a worker to open, seek, and preroll an optimistic lossless decoder output while the previous
succession session, transport subject, snapshot, and audio generation remain
current.
The lower preparing observation may mark an in-progress commit, but it does not
replace the current public snapshot with a `Preparing` transport snapshot.

Completion returns to the callback executor. Succession revalidates its pending
candidate, live source, and membership; transport re-resolves the track and
compares `PlaybackInput`; Engine revalidates playback generation, route, and
start context. Player task-handle cancellation and the callback-resumption
stop-token checkpoint prevent a superseded worker completion from reaching
those acceptance checks. Only then may Engine adopt the prepared token and
commit it: the commit retires the old render target and opens the backend on its
native handle. Engine activates the optimistic source when its complete PCM mode matches the backend result; otherwise it synchronously rebuilds the decoder output in that returned mode before activation. Transport then commits the start,
succession installs the candidate and requests best-effort lookahead, and the
service publishes the settled snapshot. Current physical PCM-mode evidence is
revalidated for gapless lookahead, where lossless compatibility with the
successor signal determines whether Engine may arm a splice; it is not separate
explicit-start evidence.
Before installation, succession reapplies the current repeat and shuffle modes
to the candidate so policy changes accepted during worker preparation are not
lost.

The destructive commit has three accepted outcomes. A started receipt installs
the candidate and emits the normal started/now-playing observations. A source
that is already naturally complete returns `playbackStarted == false`, installs
the candidate provenance, emits no started/now-playing observation or failure,
and lets its queued track-ended settlement drive normal succession. If backend
activation or final decoder setup fails after Engine accepts the generation,
the receipt also reports `playbackStarted == false`: transport still installs
the candidate provenance and settles its cancellation barrier, emits no
started/now-playing observation, and lets the queued typed failure drive route
reporting or recoverable succession. This prevents a final media failure from
being relabeled as route activation and prevents the retired previous track
from remaining the semantic current subject.

Unprepared Next and Previous retain their synchronous navigation start path.

Natural advance remains driven by accepted Engine/Player evidence. Its lower
observations are coalesced into one external-settlement publication.

### Quick mutations and external settlement

Pause, resume, Preview/Final seek, guarded queued seek, synchronous guarded
final seek, volume, mute, shuffle, repeat, stop, clear, and output selection
execute inside a service commit. Lower signals
mark the commit changed; composition waits until the command returns.
`setPlaybackMode` settles its shuffle and repeat pair as one mode update.
Pause and resume publish their lower transient events only when the refreshed
transport actually enters `Paused` or `Playing`; idle and duplicate commands
publish no false transition.

An accepted final seek advances both revision identities while preserving the
playback occurrence. A guarded seek first requires its nonzero expected
occurrence, a valid current subject and positive duration, and an elapsed value
in the inclusive range `[0, duration]`. Deferred Idle restore updates its
existing resume token with the same offset normalization as an unconditional
Final seek: the exact duration endpoint becomes zero, including the offset
later consumed by resume. Active playback additionally requires the expected
opaque audio item to remain Engine-current across prepared-next disarm and RT
splice settlement. Preview applies the same occurrence, subject, duration, and inclusive-range
checks but only publishes the preview event; it never mutates audio, disarms
lookahead, or advances either seek revision. Any rejection emits no seek event
and advances no seek revision; a matched final issuance reports success even
when an audio failure later appears through the existing transport-status
channel.

Rejection is not a rollback of audio settlement or internal prepared-slot
bookkeeping. A guarded Final may settle an already-consumed RT splice and clear
the active prepared-slot marker before rejecting the retired item. Transport
retains that consumed successor's metadata for the pending advance callback;
this is not a deferred seek. Runtime disarm must precede the Engine seek so it
can retain the exact receipt for a candidate that was actually cancelled.

Subject replacement, same-subject restart, terminal idle, and successful
restore advance only the position revision. Provider, readiness, quality,
prepared-next, and natural-advance observations received outside a command
schedule one deferred publication; observations accepted in the same executor turn coalesce.
If the executor rejects that deferred task before admission, the service keeps
the last coherent snapshot and clears its scheduling marker. The next lower
observation retries publication from the then-current lower state; it does not
publish synchronously on the signal stack.

### Restore

Restore validates and prepares its cursor candidate before mutation. Transport
installs the deferred idle request, offset, volume, and mute without lower
publication; succession then installs the prepared session. `PlaybackService`
publishes the combined state when the synchronous restore command returns.

Every installed restore creates a new position anchor and playback occurrence,
including a repeated or same-subject idle restore. The restored Idle occurrence
remains valid for guarded seeks; consuming its deferred resume may install a
later occurrence for actual playback. A missing stored session installs and
publishes nothing. Restore never advances `PlaybackFinalSeekRevision`.

## Observation

Snapshot observers are ordinary callables behind the owning signal boundary (see [signal delivery](../execution/signal.md)),
so contract-fulfilling observers run synchronously in connection order.
An escaping exception enters AO fatal handling at that boundary and provides no later-observer guarantee.
Observer-issued commands join the queue.

Seek previews carry only their elapsed position and do not alter the snapshot.
Reveal requests carry the track and optional preferred view and List ids; unspecified preferences use invalid ids.
GTK and WinUI workspace coordinators prefer the supplied view, fall back to the supplied List when necessary, and leave a track hidden by the active projection unchanged rather than weakening its filter.
Events produced within a command are delivered after snapshot settlement.

## Failure and cancellation

A result-bearing command rejected because admission is closed returns
`InvalidState`; void commands after shutdown are ignored. Guarded `trySeek` and
`tryNext` use `false` for every synchronous admission or identity/input
rejection and never leave deferred work behind. The guarded queued `seek`
overload has no later result channel; execution-time rejection emits neither a
preview nor a final update. Immediate lower
validation errors return to the caller. A queued command has no later public
result channel; playback execution failures continue through the internal
recovery and notification owners.

An inspection decoder failure is asynchronous: it publishes the existing
track-open notification and leaves the previous session and snapshot unchanged.
Failure of optimistic final-decoder preparation is not published before commit; the token retains inspection evidence and the destructive commit retries against the backend's actual PCM mode.
A fallback final decoder failure after destructive commit instead follows the accepted
failed-start outcome above and may cause succession to skip that candidate. A
newer start, stop, navigation command, final seek, output change,
clear, source or membership mutation, or shutdown invalidates the pending
start. A semantic acceptance veto completes once as `Conflict`, allowing start
or lookahead completion to clear the matching pending state deterministically;
task cancellation, replacement, or teardown may suppress completion entirely.
Engine reports captured playback, route, or start-context evidence that became
stale as `Conflict`; transport treats that result as supersession and does not
publish a track-open or route-activation notification.
Lookahead preparation is best-effort: completion clears only its matching
pending bookkeeping, and stale or `Conflict` completion has no semantic effect.
The [playback cursor](cursor.md) owns current preparation-failure reroll and
boundary recovery policy.

An unexpected command or settlement exception is an invariant fault.
The service first closes admission, abandons queued commands and pending transient events, disconnects lower observations, and revokes deferred callbacks.
It then unwinds commit bookkeeping and rethrows the original exception without settling or publishing partially observed lower state.
The last successfully committed snapshot remains authoritative, and all later result-bearing commands return `InvalidState` while void commands are ignored.

A pre-admission drain-scheduling rejection is contained:
the marker is rolled back, the FIFO remains intact, and a later command
admission retries the drain without overtaking queued commands.

Command supersession is independent from prepared-next tokens, Engine item ids,
audio cancellation barriers, and persisted session state.
Calling the public playback surface off the callback executor is an invariant
fault and enters AO fatal handling before service state is read or mutated.

## Shutdown

`AppRuntime` first shuts down session persistence. It then closes
`PlaybackService`, which rejects new commands, invalidates and clears queued
commands, disconnects lower observations, and revokes deferred service tasks.
`PlaybackBootstrap` subsequently shuts down transport and Player callback
producers while succession and the remaining runtime graph are alive.

## Implementation map

- [`PlaybackService.cpp`](../../../app/runtime/playback/PlaybackService.cpp)
  owns command ordering, commit composition, publication, and the deferred-task
  gate.
- [`PlaybackTransport.cpp`](../../../app/runtime/playback/PlaybackTransport.cpp)
  owns silent explicit-start and restore installation plus lower event delivery.
- [`PlaybackSuccession.cpp`](../../../app/runtime/playback/PlaybackSuccession.cpp)
  owns candidate session installation and succession policy.
- [`AppRuntime.cpp`](../../../app/runtime/AppRuntime.cpp) owns synchronous restore
  admission and teardown order.

## Test map

- [`PlaybackServiceTest.cpp`](../../../test/unit/runtime/PlaybackServiceTest.cpp)
  protects coherent publication, observer deferral, FIFO ordering, supersession,
  terminal exception closure, scheduler rejection, and queued-command lifetime.
- [`PlaybackGuardedSeekTest.cpp`](../../../test/unit/runtime/PlaybackGuardedSeekTest.cpp)
  protects guarded-seek identity, validation, synchronous backlog rejection,
  consecutive issuance, and same-track lower/publication lag.
- [`PlaybackSuccessionLaunchTest.cpp`](../../../test/unit/runtime/PlaybackSuccessionLaunchTest.cpp),
  [`PlaybackSuccessionAdvanceTest.cpp`](../../../test/unit/runtime/PlaybackSuccessionAdvanceTest.cpp),
  and [`PlaybackSuccessionFailureTest.cpp`](../../../test/unit/runtime/PlaybackSuccessionFailureTest.cpp)
  protect asynchronous admission, failure isolation, candidate installation,
  and prepared-next correlation.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp)
  protects coherent restore, repeated-restore baselines, backlog/reentrant restore
  rejection, and deferred nested commands.
- [`PlaybackTransportTokenTest.cpp`](../../../test/unit/runtime/PlaybackTransportTokenTest.cpp)
  protects internal event delivery order across an accepted replacement.
- [`PlaybackTransportControlTest.cpp`](../../../test/unit/runtime/PlaybackTransportControlTest.cpp)
  protects pause/resume transient events against idle and duplicate commands and normalizes backend-originated volume before runtime publication, including while muted;
  fatal subprocess coverage protects off-executor snapshot, command, and event access.

## Related documents

- [Playback architecture](README.md)
- [Playback session state](../../reference/playback/session-state.md)
