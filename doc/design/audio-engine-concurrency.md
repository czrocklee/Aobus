# Audio Engine Concurrency Contract

Aobus treats audio control as thread-tolerant inside `lib/audio`. GTK and other
application layers do not take backend-specific locks.

## Engine API

`Engine` serializes application control commands internally. Concurrent calls to
`play`, `setNext`, `clearNext`, `pause`, `resume`, `stop`, `seek`,
`setBackend`, `updateDevice`, `setVolume`, and `setMuted` are applied in one
internal order. That order only guarantees safety and a coherent final state; it
does not express user-intent priority when two commands race.

Query methods such as `status`, `transport`, `backendId`, `routeStatus`,
`volume`, `isMuted`, and `isVolumeAvailable` are safe to call concurrently. They
return self-consistent snapshots protected by Engine state locking, but they are
not linearized with in-flight control commands. A caller may observe an
intermediate transport state such as Opening or Buffering.

Backend callbacks may arrive from backend event threads, render threads, or
decoder error threads while application control commands are running. Engine
associates render callbacks with an open render session; callbacks from retired
sessions are ignored so a late callback from an old backend cannot modify the
state of a newly opened backend.

Backend/source events that can update Engine state or call user callbacks are
handed off to Engine's internal event worker. Backend event threads and decoder
threads enqueue these events and return; they do not run Engine lifecycle logic
or application callbacks inline. User callbacks registered through Engine may
call Engine control methods without re-entering the backend/source callback
stack. They must return promptly because the single Engine event worker invokes
notifications inline after applying each event.

Natural track advance uses the same event worker. The splice the render callback
performs at EOS is **wait-free**: it takes no lock, allocates nothing, and does
no unbounded work. It consumes the lookahead render cursor with one atomic
exchange, publishes the successor `TrackNode*` as the active render cursor, and
hands a non-owning splice signal to the event worker over a bounded lock-free
single-producer/single-consumer ring (with a counting-semaphore wake, so there
is no lost-wakeup and no mutex on the render thread). The current and lookahead
nodes already live in Engine's control-plane timeline; the event worker or a
settling control command promotes the lookahead node to current, destroys the
retired node (joining its decode thread off the render thread), refreshes the
current-track format, and publishes the new current input and route snapshot
before invoking callbacks. For one natural transition, a successful splice emits
`onTrackAdvanced`; the drain fallback emits `onTrackEnded`.

That ring has two kinds of consumers, both serialized by the control lock: the
event worker pops and applies signals under it, and every public control command
**settles** pending splice signals at entry before its body runs. Settling closes
the splice publication window — between the render thread's active-cursor publish
and the splice signal application, the status fields and current-track format
snapshot still describe the retired track. Pending drain-complete signals are
different: a control command may retire or reposition the render session, so
command-entry consumption forwards them to the normal event queue instead of
materializing callbacks early. The event worker later rechecks the render
generation and drain epoch before emitting terminal notifications.
Callback notifications produced by a control-thread splice settle are not run on
that thread; they are forwarded to the event worker, so user callbacks keep a
single origin thread and their relative order. Query methods do not settle: as
documented above, they may still observe the pre-advance snapshot until the
worker or the next control command applies it.

The render thread's steady-state path (`renderPcm` and `onPositionAdvanced`) is
entirely lock-free and allocation-free; the once-per-track splice above is the
only render-time transition work, and it is wait-free.
When a single render call crosses a gapless boundary, `renderPcm` may return
bytes that contain both the retired track tail and the successor head; backends
must report progress using `RenderPcmResult::positionFrameOffset` and
`RenderPcmResult::positionFrames`, not with `bytesWritten / frameSize`, so the
retired tail is not counted against the new active item even when a backend such
as ALSA commits only a prefix of the rendered buffer.

Engine state-change callbacks use the same event worker for asynchronous
backend/source events. Public Engine control commands still execute
synchronously on the caller's thread today; direct Engine callers may query after
the command returns. `Player` marshals its asynchronous reactions (Engine
state/route/terminal events, provider device events, and provider graph events)
onto the `async::Executor` it is constructed with before touching executor-owned
Player state. Layers that aggregate `Player` (e.g. `PlaybackService`) receive
Player callbacks already on their executor thread rather than on Engine's worker
or provider callback threads.

`Player` uses a shared teardown gate as a queued-task neutralizer: a task still
queued when `~Player` begins sees the closed gate and returns without touching
`Player` internals. `Player` public methods and teardown are executor-owned, and
the executor must outlive the `Player`. User callbacks run on that executor and
must return promptly; they may destroy `Player` reentrantly as long as the
destruction also occurs on the executor thread.

## Playback Input Boundary

`lib/audio` owns audio transport and output coordination, not library/UI
metadata. `audio::PlaybackInput` therefore carries only the information needed to
open and negotiate audio playback: file path, optional duration, and optional
format hints. It does not carry track ids, title, artist, album, or cover-art
ids, and `Player::Status` does not echo now-playing metadata back up the stack.

Runtime layers that need now-playing identity use `PlaybackService::PlaybackRequest`:
it pairs the narrow `audio::PlaybackInput` with the track id and display metadata
owned by the library/runtime layer. `PlaybackService` publishes those fields in
`PlaybackState`; audio status remains focused on transport, output, route graph,
quality, and volume.

Terminal events such as backend errors, source errors, and drain completion are
therefore asynchronous relative to the backend/source callback that reported
them. Until the worker applies the event, `status()` may still show the previous
transport state. Session generation checks still apply at event processing time,
so stale terminal events from retired sessions are discarded.

## Gapless Transition Ownership

`Engine::play` and `Engine::setNext` take caller-supplied opaque playback item
ids. The engine never interprets those ids; it returns the same id in
`onTrackAdvanced` so runtime layers can commit their own metadata without an
engine-generated token mapping.

`Engine::setNext` opens the next session on the control thread using the same
`TrackSession` path as `play`, decides gapless capability there (see below), and,
when capable, stores the successor as a lookahead `TrackNode` in Engine's small
timeline while arming a raw node pointer in the RT render cursor. `Engine::clearNext`
discards the lookahead node if the render thread has not consumed it and returns
the disarmed item id to the caller; if the successor has already been consumed
for a splice, it returns empty so upper layers keep metadata for the pending
`onTrackAdvanced` callback. `play`, `stop`, `seek`, and output-device changes
clear any lookahead successor because they represent explicit transport or route
changes. If the render thread
consumes the lookahead cursor, it publishes only the successor node pointer and a
non-owning splice signal; node ownership remains in the Engine timeline until
the event worker or a control command settling pending signals applies the
splice. Prepared-session source failures clear the lookahead node without
changing the current track; once a successor has been spliced, its source
generation becomes the active generation and later source errors are handled as
current-track failures.

The gapless gate is evaluated at **arm time on the control thread**, not on the
render thread. The current-track format only changes on a splice (which consumes
the lookahead cursor) or on a control command (which clears it), so the verdict
computed when the successor is armed stays valid until the render thread consumes
it. This is what keeps the render-thread splice free of any format read or
`canSplice` call.

`Player` forwards prepared-next commands to `Engine` and treats
`onTrackAdvanced` as a new playback generation before route callbacks for the
new track are captured. `PlaybackService` owns the runtime metadata for the
prepared request keyed by the opaque item id it supplied to Engine; when the
player reports a natural advance with that item id, the service commits that
request to now-playing state and emits `NowPlayingChanged` without emitting idle.
When a control command clears a still-armed successor, `PlaybackService` removes
only the returned disarmed item id from its prepared metadata; metadata for an
already-spliced item remains until the advanced callback consumes it.
If the engine takes the drain fallback instead, the existing idle path remains
responsible for asking the queue to start the next track explicitly.

`PlaybackQueueSession` owns queue policy. `peekNext()` decides and records one
pending successor, including shuffle choices, without moving the current cursor.
The cursor is committed only after a matching now-playing change arrives from a
natural advance or after an explicit fallback/manual play succeeds; then the
model prepares the following successor. If a final seek or output-device change
invalidates Engine's prepared resource while the queue remains active, the queue
prepares the pending successor again. The queue's fallback is additionally
guarded by terminal track transport (`Idle` or current-track `Error`), so source
decode/runtime errors can skip to the next track while a stale terminal
notification cannot advance the cursor once a newer track is already playing.

At EOS, the render path attempts a splice only when both the current and
prepared sessions are gapless-capable and their negotiated backend formats are
identical. Current phase gapless-capable means a lossless FLAC, ALAC, or WAV
decoder session. Lossy sessions, unknown codecs, and backend-format mismatches take the
existing drain path, which stops and closes the backend after drain completion.
No resampling, channel remapping, or artificial silence is introduced to force a
splice.

## Runtime Publication

`PlaybackService` is **executor-affine**: every public mutator, `state()`, and
the subscription-registration methods (`onXxx`) must be called on the executor's
owning thread. The contract is enforced by an always-on guard (`ensureOnExecutor`):
a cross-thread call would silently corrupt `PlaybackState` and the signal handler
storage in a release build, so a violation logs a critical message and aborts
rather than degrading to an unobserved data race. The check is a cheap thread-id
comparison (`Executor::isCurrent()`). The `Subscription` handles they return must
likewise be reset on that thread, since they mutate the same unguarded signal
handler storage that `emit` walks.
`PlaybackState` is written in exactly two places —
control commands, and the `Player` callbacks that `Player` marshals onto that same
executor — so confining all callers to one thread upholds a single-writer model
with no locking, and the service holds no internal gate of its own (teardown
drains through `Player`).

Because of that affinity, control commands publish synchronously: after calling
`Player` they refresh the `PlaybackState` snapshot and emit their command-specific
signals inline, so `state()` reflects the change as soon as the call returns.
Only the asynchronous `Player` callbacks (backend/source events originating on
Engine's event worker) are deferred — `Player` queues them on the executor — so
those are observed on a later executor turn.

`ImmediateExecutor` reports `isCurrent() == true` unconditionally and runs
`dispatch` inline, so CLI/test hosts that use it must remain effectively
single-threaded with respect to control; it provides no thread confinement of its
own.

## Backend Responsibilities

`Backend` implementations protect their own native handles. Engine serializes
application control commands before calling backend public methods, but backend
callbacks can still be in flight while public methods run. A backend must
therefore make public method / callback interleavings safe for its native API.

`close()` is the render-target lifetime boundary. After `close()` returns, the
backend must not call the `RenderTarget` passed to `open()`, and callbacks that
were already in flight for that target must have returned.

`stop()` stops active rendering but does not revoke the current target. Seek-like
flows can call `stop()`, `flush()`, and `start()` without reopening the backend.
Unrecoverable backend errors must also quiesce their own render loop or enter a
bounded retry path; Engine does not synchronously call `stop()` from a backend
error callback.

Backends must not hold locks that public methods also need while invoking
`RenderTarget` callbacks. Engine hands non-realtime events to its own worker,
but callback/native-lock reentrancy inside the backend can still block shutdown.

The render→event signal ring is single-producer: splice signals are pushed from
`RenderTarget::renderPcm`, and `onDrainComplete` pushes onto the same ring, so a
backend must not deliver drain completion concurrently with a render callback.
Reporting it from a different thread (PipeWire's loop thread versus its data
thread) is fine as long as the drained event is ordered after the last render
callback through the backend's own synchronization — which stream drain semantics
already provide, since draining stops data requests before the drained event
fires.

## Realtime Path

Render callbacks must stay free of Engine control locks. The realtime-visible
path reads the active source through `RenderTimeline` cursor atomics and uses atomics
for counters such as position and underruns. Control-plane locks are used around
lifecycle and status publication, not inside the PCM copy loop.

Gapless source switching is the only render-time cursor change, and the render
thread only publishes the successor's raw node pointer there — it never mutates
an owning `shared_ptr` and never transfers node ownership. The retired node
stays alive until the ring consumer — the event worker, or a control thread
settling at command entry — applies the splice, promotes the lookahead node to
current, and destroys the retired node; because `renderPcm` reads the active node
through the render cursor on each loop, no PCM read can race that destruction.
Retirement therefore happens **once per splice, off the render thread**, so
decode threads and file handles are reclaimed promptly and do not accumulate
across a continuous gapless run. The render path does not stop, close, reopen,
or restart the backend when the splice gate passes.

## Properties

Volume and mute are Engine-level runtime state. A backend that accepts property
writes before a native stream is open must replay the cached values when it
opens a new stream. PipeWire does this by storing volume and mute atomically and
applying them after stream connection.
