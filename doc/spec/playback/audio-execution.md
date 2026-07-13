---
id: playback.audio-execution
type: spec
status: current
domain: playback
summary: Defines Engine control serialization, event delivery, realtime rendering, gapless transitions, generation fences, Player marshalling, backend lifetime, and shutdown.
---
# Audio execution and concurrency

## Scope

This specification defines current concurrency and lifetime behavior below the application playback authorities: Engine control serialization, event-worker delivery, realtime render rules, explicit and gapless transitions, generation correlation, Player executor marshalling, backend responsibilities, and shutdown.

Succession policy belongs to the [playback cursor specification](cursor.md), decoder behavior belongs to the [decoder session specification](decoder-session.md), and cross-layer ownership belongs to the [playback](../../architecture/playback.md) and [runtime execution](../../architecture/runtime-execution.md) architectures.

## Code boundary

This contract belongs primarily to the **Core libraries** layer in the [system architecture](../../architecture/system-overview.md), under the [playback architecture](../../architecture/playback.md).
`include/ao/audio/` and `lib/audio/` own Engine, Player, sources, and backend contracts; application runtime consumes Player through executor-affine playback services and never takes backend-specific locks.

## Terminology

- **Control order**: the internal serialization order of concurrent Engine commands.
- **Event worker**: Engine's single non-realtime worker that applies backend/source/render events and invokes Engine callbacks.
- **Render session**: one backend attachment correlated to a render generation.
- **Timeline node**: Engine-owned current or lookahead audio item and its source.
- **Splice signal**: the non-owning realtime notification that a prepared lookahead became active at end of stream.
- **Callback-generation floor**: the minimum audio generation whose materialized callbacks may still begin after a synchronizing receipt.
- **Player graph epoch**: Player's current provider/route graph identity used when accepting marshalled route state.

## Invariants

- Engine control methods are safe under concurrent calls and are applied in one internal order.
- Query snapshots are self-consistent but need not be linearized with an in-flight command or pending splice.
- Backend, decoder, and realtime callbacks never run Engine lifecycle logic or application callbacks inline.
- The steady-state realtime render path is lock-free and allocation-free.
- A natural splice is wait-free and transfers no owning pointer on the render thread.
- Timeline-node destruction and decode-thread joins happen off the render thread exactly once.
- Engine callbacks have one origin thread: the event worker.
- Player state and public methods have one owner executor; lower callbacks are marshalled before touching that state.
- A callback from a retired render/audio/provider generation cannot mutate a newer state generation.
- `close()` revokes a backend's render target; no render callback begins after it returns.
- Shutdown closes callback admission and joins producers before their targets are destroyed.

## State model

Engine owns a lifecycle (`Running`, shutting down, shut down), one serialized control plane, one event queue/worker, backend and route state, a render session/generation, current and optional lookahead timeline nodes, an atomic realtime cursor, a bounded single-producer/single-consumer render-to-event ring, transport/properties, and callback-generation fences.

The render cursor points non-owningly into Engine's control-plane timeline.
The current and lookahead nodes keep source, decoder, and file lifetime alive until a control/event consumer retires them.

Player owns one Engine, all registered providers, route graph state, its executor, and a shared callback/teardown gate.
PlaybackService owns runtime metadata separately; `audio::PlaybackInput` contains only path, optional duration, and optional format hints.

## Commands and transitions

### Control and query serialization

Concurrent calls to `play`, `stagePlayback`, `commitPlayback`, `setNext`, `clearNext`, `pause`, `resume`, `stop`, `seek`, `setBackend`, `updateDevice`, `setVolume`, and `setMuted` enter the Engine control serialization.
This order guarantees safety and a coherent final state, not user-intent priority between racing callers.

Queries including status, transport, backend id, route state, volume, mute, and availability are safe concurrently.
They do not settle a pending splice and may observe an intermediate transport or the pre-splice snapshot until the event worker or a control command applies it.

Every public control command settles pending splice signals at entry under the control lock.
This closes the window after the realtime cursor changed but before status and current-format state caught up.
Callbacks produced by a control-thread settle are forwarded to the event worker rather than invoked on the caller thread.

Pending drain-complete signals are not materialized by command entry.
A command may retire or reposition their render session, so they enter the normal event queue and are rechecked against render generation and drain epoch before notification.

### Event delivery and reentrancy

Backend events, decoder errors, render transitions, and provider changes enqueue and return.
The event worker applies each event, refreshes state, and invokes callbacks inline afterward.

Engine callbacks may call supported Engine control methods because they are no longer inside the backend/source callback stack.
They must return promptly and cannot call `Engine::shutdown()` or destroy the Engine from the notification; those operations are deferred until the callback returns.

Player repeats this separation at the application boundary.
Engine/provider callbacks queue work onto Player's executor, and Player callbacks to application runtime run from that executor.
Queued work first checks the shared teardown gate.
The current non-realtime Engine event deque and Player executor-task stream have no combined capacity or coalescing contract; [RFC 0028](../../rfc/0028-bounded-audio-observation-delivery.md) proposes classified bounded delivery.

### Realtime rendering and natural end

`renderPcm` and `onPositionAdvanced` take no Engine control lock and allocate no memory in steady state.
They read the active source through timeline cursor atomics and use atomics for position and underrun counters.

At end of stream, the render path may consume one armed lookahead pointer with an atomic exchange, publish that node as active, and push one non-owning splice signal into the bounded SPSC ring.
A counting semaphore wakes the consumer without a lost wakeup or render-thread mutex.

The event worker or next control command promotes the lookahead node, retires the old node, refreshes current format and route/current-input snapshots, then schedules callbacks.
A successful transition emits `onTrackAdvanced`; a drain fallback emits `onTrackEnded` after generation/epoch checks.

One render call may contain retired-track tail and successor head.
Backends report progress with `RenderPcmResult::positionFrameOffset` and `positionFrames`, not `bytesWritten / frameSize`, so committed tail bytes are not counted as successor position.

The ring is single-producer across render splice and drain-complete publication.
A backend may report drain completion from another thread only when its own synchronization orders it after the final render callback.

### Prepared lookahead

`setNext` opens the candidate through the same track-session path as `play` on the control thread.
If the candidate and current route are gapless-capable, Engine retains the node as lookahead and arms its raw pointer for realtime consumption.

The gapless verdict is fixed at arm time.
The current negotiated format cannot change without consuming or clearing lookahead, so the render thread performs no format read or capability test.

`clearNext` returns the disarmed opaque item id when the render thread has not consumed it.
If already consumed, it returns empty and upper runtime retains matching metadata for the later advanced callback.
Explicit `play`, `stop`, `seek`, and output-device changes clear unconsumed lookahead.
Prepared-source failure clears lookahead without changing the current track; after splice, that source generation is current and fails as current.

A splice is permitted only when both sessions are gapless-capable and negotiated backend formats are identical.
Current gapless-capable codecs are lossless FLAC, ALAC, and WAV.
Lossy/unknown codecs or format mismatch drain and close; no resampling, channel remapping, or artificial silence forces compatibility.

### Explicit staged start

`stagePlayback` opens a source without publishing it to the render timeline.
`commitPlayback` publishes only if the active generation and start context still match.

Engine keeps a weak staged-source registry so a candidate decode failure is not discarded as a stale timeline event.
If the event worker wins, it latches the original error and commit returns it before changing callback floor, lookahead, or active source.
If commit wins, the staged registration is removed while publishing, and an already queued source error is reduced exactly once as an active failure.

A nonzero staged offset is applied before `StreamingSource` starts its background thread.
An error from a discarded pre-seek epoch therefore cannot invalidate the healthy candidate.

### Identity and callback fences

Engine treats playback item ids as opaque and returns the caller's id in natural-advance events.
`TrackAdvanced`, `PlaybackFailure`, `RouteStatus`, and `TrackEnded` also carry the originating audio generation.

A successful explicit-start or completed-stop receipt raises the callback-generation floor and synchronizes callback delivery.
A callback from a covered generation cannot begin after that receipt returns.

Player repeats the generation test when the queued executor task runs.
An accepted route snapshot binds to the Player graph epoch observed on that executor turn; it is not inferred from when the Engine callback was queued.
FIFO delivery permits an earlier advanced task to reset the graph before a following route task is interpreted.

PlaybackService retains the prepared runtime request by opaque item id.
Natural advance commits matching now-playing metadata without an intermediate idle publication.
When clear returns a disarmed id, only that metadata is removed; metadata for an already spliced item remains until the advanced callback consumes it.

### Runtime publication

PlaybackService public mutators, state reads, subscription creation, and subscription reset are executor-affine.
An always-on `ensureOnExecutor` guard logs critically and aborts on violation rather than permitting an unobserved race.

Control commands call Player, refresh `PlaybackState`, and emit command-specific signals synchronously.
Asynchronous Player events arrive on a later executor turn.
Signal handlers defer service destruction until publication returns.

`ImmediateExecutor` reports current unconditionally and dispatches inline.
Hosts using it must remain effectively single-threaded; it supplies no confinement.

### Backend lifetime and properties

Backends protect native handles against public-method/callback interleavings.
They do not hold locks needed by public methods while invoking `RenderTarget` callbacks.

`stop()` stops rendering without revoking the target, permitting stop/flush/start seek flows.
`close()` is the revocation boundary and waits for in-flight target callbacks.
An unrecoverable backend error quiesces its render loop or enters a bounded retry; Engine does not synchronously call stop from the backend error callback.

Volume and mute are Engine runtime state.
A backend accepting properties before stream open caches and reapplies them when the stream becomes live.

### Shutdown

The first `Engine::shutdown()` caller changes lifecycle under the control lock, retires the render session, stops and joins the event worker, and closes backend/timeline state.
Commands admitted after lifecycle transition do not enter backend/timeline logic and result-bearing commands return `InvalidState`.
Concurrent shutdown callers wait for the single teardown; repeated completed shutdown is a no-op.

Player public methods and destruction run on its executor, which outlives Player.
Destruction closes the shared gate before providers and Engine stop, so already queued tasks return without touching Player state.
`BackendProvider::shutdown()` is `noexcept`; after it returns, provider-owned asynchronous sources cannot initiate new device or graph callbacks.

## Failure and cancellation

External media, device, route, and capability failures cross public audio boundaries as `Result` or asynchronous typed events.
Stale generation events are discarded.
Terminal events remain asynchronous relative to the producer callback, so queries may briefly show the earlier transport.

Engine control has no general stop-token cancellation.
Decoder/source workers and runtime orchestration own their more specific cancellation.
Shutdown is the terminal lifetime operation and must not originate from an Engine/Player notification stack.

Realtime ring overflow and violated single-producer assumptions are invariant failures, not recoverable media outcomes.

## Persistence and versioning

This execution contract persists no state.
Playback-session persistence samples application succession and transport intent through its own specification; Engine generations, timeline nodes, staged candidates, route epochs, and prepared lookahead are transient.

## Frontend observations

Frontend and UIModel observe executor-affine runtime snapshots and callbacks, never Engine/backend threads.
`PlaybackInput` and Engine status contain no track title, artist, album, or cover identity.
Runtime `PlaybackRequest` and `PlaybackState` own that application metadata.

Transient Opening/Buffering/Seeking and deferred terminal changes are valid observations.
Frontends do not add locks around backend calls or reconstruct gapless/succession behavior.

## Implementation map

- [`Engine.h`](../../../include/ao/audio/Engine.h) and [`Engine.cpp`](../../../lib/audio/Engine.cpp) own control, event, timeline, render, generation, and shutdown behavior.
- Audio detail timeline, track-session, and streaming-source code under [`lib/audio/detail/`](../../../lib/audio/detail/) owns nodes and decode lifetime.
- [`Player.h`](../../../include/ao/audio/Player.h) and [`Player.cpp`](../../../lib/audio/Player.cpp) own provider composition, executor marshalling, graph epochs, and teardown gate.
- [`Backend.h`](../../../include/ao/audio/Backend.h) and concrete backends under [`lib/audio/backend/`](../../../lib/audio/backend/) own native lifetime.
- [`PlaybackService.cpp`](../../../app/runtime/PlaybackService.cpp) owns executor-affine application publication and prepared metadata.

## Test map

- [`EngineConcurrencyTest.cpp`](../../../test/unit/audio/EngineConcurrencyTest.cpp) protects concurrent commands and teardown.
- [`EngineGaplessTest.cpp`](../../../test/unit/audio/EngineGaplessTest.cpp), [`EngineDrainTest.cpp`](../../../test/unit/audio/EngineDrainTest.cpp), and [`AudioBackendRenderProgressTest.cpp`](../../../test/unit/audio/backend/detail/AudioBackendRenderProgressTest.cpp) protect splice, drain, mixed-buffer progress, and fallback.
- [`EngineCallbackTest.cpp`](../../../test/unit/audio/EngineCallbackTest.cpp), [`EngineErrorTest.cpp`](../../../test/unit/audio/EngineErrorTest.cpp), and [`EngineBackendSwapTest.cpp`](../../../test/unit/audio/EngineBackendSwapTest.cpp) protect generations and stale events.
- [`PlayerTest.cpp`](../../../test/unit/audio/PlayerTest.cpp) protects executor marshalling, graph epochs, and gate behavior.
- [`StreamingSourceTest.cpp`](../../../test/unit/audio/StreamingSourceTest.cpp) protects decode worker and source retirement.
- Runtime playback tests under [`test/unit/runtime/`](../../../test/unit/runtime/) protect executor-affine publication and application metadata.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Playback succession cursor](cursor.md)
- [Decoder session](decoder-session.md)
- [Audio quality analysis](quality-analysis.md)
- [RFC 0027: serialized headless callback executor](../../rfc/0027-serialized-headless-callback-executor.md)
- [RFC 0028: bounded audio observation delivery](../../rfc/0028-bounded-audio-observation-delivery.md)
