# Audio Engine Concurrency Contract

Aobus treats audio control as thread-tolerant inside `lib/audio`. GTK and other
application layers do not take backend-specific locks.

## Engine API

`Engine` serializes application control commands internally. Concurrent calls to
`play`, `pause`, `resume`, `stop`, `seek`, `setBackend`, `updateDevice`,
`setVolume`, and `setMuted` are applied in one internal order. That order only
guarantees safety and a coherent final state; it does not express user-intent
priority when two commands race.

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

Engine state-change callbacks use the same event worker for asynchronous
backend/source events. Public Engine control commands still execute
synchronously on the caller's thread today; direct Engine callers may query after
the command returns. `Player` marshals its asynchronous reactions (Engine
state/route/terminal events, provider device events, and provider graph events)
onto the `async::IExecutor` it is constructed with before touching executor-owned
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

## Runtime Publication

`PlaybackService` is **executor-affine**: every public mutator, `state()`, and
the subscription-registration methods (`onXxx`) must be called on the executor's
owning thread. The contract is enforced by an always-on guard (`ensureOnExecutor`):
a cross-thread call would silently corrupt `PlaybackState` and the signal handler
storage in a release build, so a violation logs a critical message and aborts
rather than degrading to an unobserved data race. The check is a cheap thread-id
comparison (`IExecutor::isCurrent()`). The `Subscription` handles they return must
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

`IBackend` implementations protect their own native handles. Engine serializes
application control commands before calling backend public methods, but backend
callbacks can still be in flight while public methods run. A backend must
therefore make public method / callback interleavings safe for its native API.

`close()` is the render-target lifetime boundary. After `close()` returns, the
backend must not call the `IRenderTarget` passed to `open()`, and callbacks that
were already in flight for that target must have returned.

`stop()` stops active rendering but does not revoke the current target. Seek-like
flows can call `stop()`, `flush()`, and `start()` without reopening the backend.
Unrecoverable backend errors must also quiesce their own render loop or enter a
bounded retry path; Engine does not synchronously call `stop()` from a backend
error callback.

Backends must not hold locks that public methods also need while invoking
`IRenderTarget` callbacks. Engine hands non-realtime events to its own worker,
but callback/native-lock reentrancy inside the backend can still block shutdown.

## Realtime Path

Render callbacks must stay free of Engine control locks. The realtime-visible
path reads the active source through `RenderSourceSlot` atomics and uses atomics
for counters such as position and underruns. Control-plane locks are used around
lifecycle and status publication, not inside the PCM copy loop.

## Properties

Volume and mute are Engine-level runtime state. A backend that accepts property
writes before a native stream is open must replay the cached values when it
opens a new stream. PipeWire does this by storing volume and mute atomically and
applying them after stream connection.
