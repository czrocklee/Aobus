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

Backends must not hold locks that public methods also need while invoking
`IRenderTarget` callbacks. Engine callbacks can synchronously update playback
state, and deadlocking a backend event thread would block shutdown.

## Realtime Path

Render callbacks must stay free of Engine control locks. The realtime-visible
path reads the active source through atomics and uses atomics for counters such
as position and underruns. Control-plane locks are used around lifecycle and
status publication, not inside the PCM copy loop.

## Properties

Volume and mute are Engine-level runtime state. A backend that accepts property
writes before a native stream is open must replay the cached values when it
opens a new stream. PipeWire does this by storing volume and mute atomically and
applying them after stream connection.
