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
- Stop, replacement start, admitted next/previous, restore, output-route change, clear,
  and shutdown supersede older queued start/navigation commands. When output is unavailable, Next and Previous are rejected before they can supersede an earlier command. Queued navigation checks readiness again before touching succession. Orthogonal
  volume, mute, pause, resume, seek, shuffle, and repeat commands remain FIFO;
  occurrence-guarded seeks perform their own execution-time identity check.
- Elapsed clock drift alone is not semantic content and causes no publication.
  `elapsed()` samples nonnegative live position without publishing, bounded by the committed duration when positive.
  Runtime/audio occurrence checks before and after the sample keep it correlated to the last committed snapshot; a mismatch falls back to that snapshot's anchor.
- Every installed explicit start, replay, natural advance, and deferred restore
  receives a new nonzero playback occurrence. Refresh, pausing and resuming the same open audio item, and seeks inside a Track preserve it. Resuming a deferred subject at its held offset installs a new occurrence if the start commits. Stop invalidates the occurrence. An end-of-Track seek follows natural completion.
  Natural terminal Idle without deferred resume ownership and failure of the current audio item also invalidate it. A natural successor keeps its newly installed identity; a failed lookahead does not retire the current occurrence.
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

Runtime publishes the provider catalog and the engine-confirmed `output.selectedDevice`. Neither proves that a stream is open. Initial discovery may choose an available device and compatible profile when there is no configured route or pending request. If Player cannot configure a requested route, `output.optPendingOutputSelection` names the request; `selectedDevice` can still name the old route, but playback cannot use it as a fallback.

Entering pending stops audio on the old route and disarms lookahead. If the switch interrupted a track, transport holds that track and its elapsed position as Paused while Engine is idle. A seek within the held track can change its resume position. While the output is unavailable, Next, Previous, and a relative seek past the end cannot advance or discard that track. A rejected replacement leaves the held track intact. Stop discards the held track but not the output request.

If the active item ends or fails before the output change takes effect, Engine retains its terminal outcome, item ID, and generation. A gapless successor shares its predecessor's generation. If the route change retires both the advance and terminal notifications, Transport announces the matching prepared successor before settling its outcome. A natural end advances from the track that finished or, when the output is unavailable, saves it as the last one played. A failure is reported once for the track that failed; it does not become a paused track for Resume, and an unavailable route does not count against a healthy successor. A failure while preparing a future track is not a terminal outcome for the current track.

Reselecting the old fixed device while pending must pass discovery again. Discovery can configure the requested route but cannot restart the interrupted track; it takes an explicit resume. A device that is listed but refuses to open reports the backend failure instead. A live unplug on a fixed PipeWire stream is likewise an output failure, not a request to play on a substitute.

An empty device id denotes a provider-defined default only for a compatible route. For a named PipeWire sink, `Fixed` forbids both startup fallback and later moves. On open, PipeWire resolves `node:<node.name>` on the stream's own connection and uses the unique sink's current serial as `target.object`. This lookup and PCM negotiation share a single wait deadline; neither runs on the render thread, but the synchronous open can still block its caller. A missing or ambiguous name cannot open a fixed stream. Neither a matching display label nor a recycled registry id or serial identifies that sink. The name identifies a logical route, not hardware: a returning sink can have a new serial, and a reassigned name can denote a different sink.

With explicit `AllowSystemReroute`, Player can configure the named PipeWire request before the sink is enumerated. The session manager may route to another sink at startup or later; the configured name is not evidence of the physical destination. An exclusive request stays exclusive and may fail on a busy alternate. Rerouting requires provider support and does not authorize substitution by another backend.

The neutral selection type owns no persistence lifecycle. UIModel resolves interactive restore intent; the frontend submits it and owns checkpoint timing. See the [application configuration](../../reference/persistence/application-config.md#global-runtime-preference-group) for the saved identity, numeric-id compatibility and consent fields.

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

## Commands and transitions

### Primary Play

[`PlaybackActions`](../../../app/uimodel/playback/command/PlaybackActions.cpp) gives local and system-media commands the same Play target policy:

- During active playback (Opening, Buffering, Playing, or Seeking), the shared Play/Pause action pauses; a repeated Play does not restart the track. [MPRIS `PlayPause`](../../reference/linux/mpris.md#player-methods) selects Play or Pause from the advertised status before dispatching the action.
- Play or the start side of Play/Pause resumes a paused or deferred track first. Clearing succession does not discard a deferred subject or its offset. A pending output request blocks resume without falling back to another track.
- With no subject to resume, Idle or Error can start a track only when the output is ready. GTK, WinUI, and AppKit use the first selected track still present in the focused view's live projection. The TUI uses the track under its cursor, not its marks. A missing or filtered-out target does not fall back to the first row.

`isCapable(Play)` advertises Play during active playback even though `tryExecute(Play)` returns `false` without changing transport. Otherwise Play availability requires a resumable subject or an eligible target on a ready output. A selection change alone can enable or disable Play without a playback publication. When a delayed command reaches the frontend executor, it checks the current transport and target again: advertising Play does not reserve a track.

For a new selection start, local and system-media commands use the same frontend callback. `tryExecute` returns the callback's immediate start-admission result, not confirmation that decoder preparation finished or audio started.

Activating a row explicitly requests playback from that row rather than resuming a different track.

### Admission and execution

The command boundary covers service commits, publication turns, and automatic
succession while it handles a playback failure or resolves the next track after
natural Idle. An admitted ordinary command executes synchronously only outside
that boundary with no queued backlog. Otherwise it joins the FIFO. For example,
Next requested by a skip-report observer is evaluated after recovery settles,
not in the middle of the failure walk. One deferred task consumes one command
and schedules the next drain after settlement.

`startFromView` reports synchronous view, membership, request, readiness, and
worker-task admission. Success does not mean that decoder preparation completed
or that a new current subject was installed. When called by an observer it
initially reports successful command-queue admission; it has no separate public
completion token.

The occurrence-bearing `seek` overload is a guarded queued positioning command. It keeps
normal FIFO ordering across the command boundary and any backlog, captures
occurrence, elapsed position, and mode by value, and validates occurrence and
range in Transport when the operation executes. A stale queued Preview or Final
issues no seek and publishes no seek update; a valid queued Preview therefore retains its matching Final
settlement instead of leaving frontend preview state behind merely because an
orthogonal command temporarily owned the boundary. The original occurrence-free
`seek` remains the unconditional compatibility command.

`seekBy` captures an occurrence and signed delta in the same FIFO.
At execution it requires the matching occurrence and positive duration, samples live Transport elapsed, and clamps the relative target without overflowing before issuing a guarded Final.
Its default `Clamp` behavior issues a Final seek at the clamped target, including the end request at a positive duration; `Next` instead explicitly advances for a positive offset strictly past the live endpoint, only with an available successor and matching runtime/audio identity.
With `ClampIfChanged`, a slider step that cannot move the live position issues no Final. Repeated outward steps at a paused endpoint do not flush audio again.
The accepted Next uses existing succession cancellation of pending explicit preparation and lookahead; a rejected or ordinary relative seek does not invalidate command generations.
An exact-end target remains a Final seek and therefore an end request; unavailable or stale past-end Next is a no-op.
Sequential queued relative commands therefore accumulate from each execution's live position rather than a cached publication; headless consumers do not need rendering subscriptions.
The live query and seek are separate observations: an intervening realtime replacement is rejected by the existing Engine item guard, not treated as a new target.

`trySeek` is a synchronous-only guarded final seek. It returns `false` without
queuing when admission is closed, the command boundary is active, or the
FIFO has a backlog. Otherwise it runs through normal commit settlement
without changing the command-invalidating generation and returns whether
Transport and Engine accepted the expected occurrence/audio-item chain.

`tryNext` has the same synchronous admission rules. It requires a ready output
and validates the nonzero runtime occurrence and subject. Active playback then
asks Player and Engine whether the settled realtime timeline still exposes
Transport's expected audio item; an exact deferred occurrence (restored Idle or
a held paused position) is the no-active-audio exception. Only a match advances
the invalidating generation and enters normal synchronous Next
planning. The Engine observation releases its control lock before succession
runs. This admission observation does not replace `trySeek`'s mutation-time
identity guards.

Session restore keeps its call-level result on `AppRuntime` and is rejected
while the command boundary is active or the FIFO has a backlog.

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
Preparation briefly returns to the callback executor so Engine can revalidate its captured route and obtain the selected Backend's non-blocking prewarm hint, then resumes on a worker to prepare an optimistic lossless decoder output while the previous succession session, transport subject, snapshot, and audio generation remain current. Ordinary offsets are sought and prerolled during preparation; an [extended decoder seek](audio-execution.md#streaming-decode-and-buffering) runs after source activation.
The lower preparing observation may mark an in-progress commit, but it does not
replace the current public snapshot with a `Preparing` transport snapshot.

Completion returns to the callback executor. Succession revalidates its pending
candidate, live source, and membership; transport re-resolves the track and
compares `PlaybackInput`; Engine revalidates the playback generation and the
output route's identity and revision. Player task-handle cancellation and the callback-resumption
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
the candidate and emits `Started` and the now-playing observation. `Started`
means playback was accepted; it does not mean the backend is already rendering. A source
that is already naturally complete returns `playbackStarted == false`, installs
the candidate provenance, emits no started/now-playing observation or failure,
and lets its queued track-ended settlement drive
[cursor succession](cursor.md#failure-and-cancellation), including when no
eligible track remains after clean ends at offset zero. If backend
activation or final decoder setup fails after Engine accepts the generation,
the receipt also reports `playbackStarted == false`: transport still installs
the candidate provenance and settles its cancellation barrier, emits no
started/now-playing observation, and lets the queued typed failure drive route
reporting or recoverable succession. This prevents a final media failure from
being relabeled as route activation and prevents the retired previous track
from remaining the semantic current subject.

Unprepared Next and Previous retain their synchronous navigation start path.

Natural advance remains driven by accepted Engine/Player evidence.
When failure recovery or natural advance runs outside a service commit, its
lower observations follow the usual
[deferred snapshot publication](#quick-mutations-and-external-settlement).
Finishing the transition allows queued commands to drain; it does not itself
publish a snapshot.

### Quick mutations and external settlement

Pause, resume, Preview/Final seek, guarded queued seek, synchronous guarded
final seek, volume, mute, shuffle, repeat, stop, clear, and output selection
execute inside a service commit. Lower signals
mark the commit changed; composition waits until the command returns.
`setPlaybackMode` settles its shuffle and repeat pair as one mode update.
Pause emits `Paused` only when the transport enters `Paused`. Resuming active playback from `Paused` emits `Started` whether the transport enters `Playing` immediately or returns to `Buffering` to wait for PCM. When that occurrence becomes ready, the service publishes `Playing` without a second `Started`. Idle and duplicate commands emit no false transition.

An accepted Final seek advances both revision identities. A target inside the Track preserves its playback occurrence.
An unguarded Final clamps its target to `[0, duration]`; a guarded seek first requires its nonzero expected occurrence, a valid current subject, positive duration, and a target in that inclusive range, rejecting a target past the duration.
Active playback additionally requires the expected opaque audio item to remain Engine-current across prepared-next disarm and RT splice settlement, including for an end request.
Preview applies the guarded occurrence, subject, duration, and range checks but only publishes a position; it never mutates audio, disarms lookahead, ends the Track, or advances either seek revision.
A rejected seek publishes no Preview or Final update and advances neither the position revision nor the final-seek revision. A successful `trySeek` reports issuance, not audio completion; a later audio failure is reported through transport status.

A guarded seek's optional settlement callback runs after any resulting publication, even on rejection. It can run inline, follows observer reentrancy rules and reports neither acceptance nor audio completion. Shutdown abandons callbacks already in the queue. A seek submitted after admission closes does not run, but its callback runs immediately.

A Final seek to a **positive** duration is a transport end request, not a decoder seek.
While Playing it cancels prepared audio through an item-guarded stop barrier and completes the subject through natural Idle succession, without emitting Stopped or replaying a successor whose splice already won.
While Paused, or Idle with deferred resume ownership, it holds the endpoint; resume completes the subject without opening a decoder at the endpoint.
Seeking back inside drops the end intent, retains the paused/deferred state, and resumes at the new offset.
Resuming any deferred playback, held or restored, reads its Track again, so a Track relinked in the meantime opens its current file; a Track the library no longer resolves keeps the held request.
The held millisecond position itself carries that intent, so a nonpositive duration has no end request: zero remains the start of an unknown-duration file or a nonempty sub-millisecond segment.
Session capture/restore separately normalize a saved endpoint to zero; that persistence rule does not reset a live endpoint seek.
Whole-file codecs and segments obey the same application policy, while the [segment decoder](decoder-session.md#segment-playback) retains frame-exact tail delivery at its floored duration.

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
Ordinary observer-issued commands follow the [command admission rules](#admission-and-execution).

A seek preview carries an elapsed position without changing the snapshot. Canceling a gesture never issues a seek. On cancellation or a rejected Final, the service notifies `onSeekPreviewEnded` subscribers only if that gesture still owns the preview on the current occurrence. The slider and clock can then resample live elapsed without changing either seek revision or clearing a newer preview.

Playing-track reveal requests carry the subject's track and source List ids. GTK, TUI and WinUI use [source-first workspace navigation](../workspace/navigation.md#navigate-to-the-playback-source) before selecting or scrolling to the row. AppKit does not handle this playback reveal; its Reveal in Finder action locates a file instead.

Explicit reveals, such as album navigation or GTK startup restore, carry the track and optional preferred view and List ids (invalid when unspecified). GTK and WinUI prefer the view, use the supplied List as a fallback, and do not weaken the target view's filter to expose a hidden row.

**Known limitation:** GTK uses the List hint to find an open page only when no view id was supplied. If a supplied view has already closed, GTK cannot reveal the row through the List hint. WinUI can reuse or open a view for that List.

Events produced within a command are delivered after snapshot settlement.

## Failure and cancellation

A result-bearing command rejected because admission is closed returns
`InvalidState`; void commands after shutdown are ignored. Guarded `trySeek` and
`tryNext` use `false` for every synchronous admission or identity/input
rejection and never leave deferred work behind. Immediate lower validation
errors return to the caller. A queued command has no later public result
channel; playback execution failures continue through the internal recovery
and notification owners.

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
Engine reports a stale playback generation or output route as `Conflict`; transport treats that result as supersession and does not
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

Service shutdown and destruction require the callback executor. Owners must
defer teardown until any active commit, publication, or automatic succession
transition returns.

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

- [`PlaybackServiceTest.cpp`](../../../test/runtime/PlaybackServiceTest.cpp)
  protects coherent publication, observer deferral, FIFO ordering, supersession,
  terminal exception closure, scheduler rejection, and queued-command lifetime.
- [`PlaybackFailureObserverTest.cpp`](../../../test/runtime/PlaybackFailureObserverTest.cpp)
  checks that Next and clear requested by a grouped-skip observer act on the
  recovered track, and that Next requested at the failure limit leaves playback
  stopped.
- [`PlaybackGuardedSeekTest.cpp`](../../../test/runtime/PlaybackGuardedSeekTest.cpp)
  protects guarded-seek identity, validation, synchronous backlog rejection,
  consecutive issuance, and same-track lower/publication lag.
- [`PlaybackSuccessionLaunchTest.cpp`](../../../test/runtime/PlaybackSuccessionLaunchTest.cpp),
  [`PlaybackSuccessionAdvanceTest.cpp`](../../../test/runtime/PlaybackSuccessionAdvanceTest.cpp),
  and [`PlaybackSuccessionFailureTest.cpp`](../../../test/runtime/PlaybackSuccessionFailureTest.cpp)
  protect asynchronous admission, failure isolation, candidate installation,
  and prepared-next correlation.
- [`PlaybackSessionTest.cpp`](../../../test/runtime/PlaybackSessionTest.cpp)
  protects coherent restore, repeated-restore baselines, backlog/reentrant restore
  rejection, and deferred nested commands.
- [`PlaybackTransportTokenTest.cpp`](../../../test/runtime/PlaybackTransportTokenTest.cpp)
  protects internal event delivery order across an accepted replacement.
- [`PlaybackTransportControlTest.cpp`](../../../test/runtime/PlaybackTransportControlTest.cpp)
  protects pause/resume transient events against idle and duplicate commands and normalizes backend-originated volume before runtime publication, including while muted;
  fatal subprocess coverage protects off-executor snapshot, command, and event access.

## Related documents

- [Playback architecture](README.md)
- [Playback session state](../../reference/playback/session-state.md)
