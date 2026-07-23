---
id: playback.audio-execution
type: spec
status: current
domain: playback
summary: Defines Engine control serialization, streaming PCM buffering, event delivery, realtime rendering, gapless transitions, generation fences, Player marshalling, backend lifetime, and shutdown.
---
# Audio execution and concurrency

## Scope

This specification defines current concurrency and lifetime behavior below the application playback authorities: Engine control serialization, streaming PCM buffering, event-worker delivery, realtime render rules, explicit and gapless transitions, generation correlation, Player executor marshalling, backend responsibilities, and shutdown.

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
- **Buffer target**: a requested buffered duration converted to a rounded-up byte count and capped by the PCM ring capacity.
- **Predictive block headroom**: writable ring capacity reserved from the previous nonempty decoded block size before requesting the next block.
- **Render quiescence**: the `Backend::stop()` postcondition that closes render admission and waits for the active render cycle and its render notifications to return.

## Invariants

- Engine control methods are safe under concurrent calls and are applied in one internal order.
- Query snapshots are self-consistent but need not be linearized with an in-flight command or pending splice.
- Backend, decoder, and realtime callbacks never run Engine lifecycle logic or application callbacks inline.
- The steady-state realtime render path is lock-free and allocation-free.
- A natural splice is wait-free and transfers no owning pointer on the render thread.
- Timeline-node destruction and decode-thread joins happen off the render thread exactly once.
- Preroll and background decode use the same capacity-bounded byte-target policy.
- The source requests another decoded block only while below its buffer target and with predictive block headroom; one decoded block cannot exceed the whole PCM ring.
- Engine callbacks have one origin thread: the event worker.
- Player state and public methods have one owner executor; lower callbacks are marshalled before touching that state.
- A callback from a retired render/audio/provider generation cannot mutate a newer state generation.
- `stop()` establishes render quiescence without revoking the backend's open target.
- `close()` revokes a backend's render target; no render callback begins after it returns.
- PCM ring reset runs only while its producer, render consumer, and buffered-duration observer are quiescent.
- Shutdown closes callback admission and joins producers before their targets are destroyed.

## State model

Engine owns a lifecycle (`Running`, shutting down, shut down), one serialized control plane, one event queue/worker, backend and route state, a render session/generation, current and optional lookahead timeline nodes, an atomic realtime cursor, a bounded single-producer/single-consumer render-to-event ring, transport/properties, and callback-generation fences.

The render cursor points non-owningly into Engine's control-plane timeline.
The current and lookahead nodes keep source, decoder, and file lifetime alive until a control/event consumer retires them.

Player owns one Engine, all registered providers, route graph state, its executor, and a shared callback/teardown gate.
PlaybackTransport owns runtime metadata separately; `audio::PlaybackInput` contains only path, optional duration, and optional format hints.

## Commands and transitions

### Control and query serialization

Concurrent calls to `play`, `stagePlayback`, `commitPlayback`, `setNext`, `clearNext`, `pause`, `resume`, `stop`, `seek`, `setBackend`, `updateDevice`, `setVolume`, and `setMuted` enter the Engine control serialization.
This order guarantees safety and a coherent final state, not user-intent priority between racing callers.
The synchronous compatibility forms of `stagePlayback` and `setNext` retain
that serialization across decoder open, activation, and registration or
lookahead publication. Only their explicit asynchronous capture/prepare/adopt
forms release serialization while worker preparation is pending and therefore
revalidate captured evidence during adoption.

The complete `status()` snapshot enters control serialization because it observes the current source's PCM queue.
It waits for an in-flight command such as seek to finish before reading buffered duration, but unlike control-command entry it does not settle pending realtime signals and may still return the pre-splice snapshot.

Scalar state-only queries including transport, backend id, route state, volume, mute, and availability use narrower state or atomic synchronization and remain safe concurrently.
They may observe an intermediate transport until the active control command or event worker publishes its next state.

Every public control command settles pending splice signals at entry under the control lock.
This closes the window after the realtime cursor changed but before status and current-format state caught up.
Callbacks produced by a control-thread settle are forwarded to the event worker rather than invoked on the caller thread.
The narrower transition-state lock is held only while copying or comparing
current format evidence. Disarming or publishing lookahead and settling
realtime splice signals occur outside that lock because splice settlement also
updates the same transition state.

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
The current non-realtime Engine event deque and Player executor-task stream have no combined capacity or coalescing contract.

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

### Streaming decode and buffering

`StreamingSource` converts each duration target to bytes by rounding fractional bytes upward and capping the result at the fixed PCM ring capacity.
This makes a high-rate target reachable even when its requested duration represents more data than the ring can hold.
Initial preroll, post-seek preroll, and the background decode loop all use this byte policy.

Before reading another decoder block, the sole producer checks both that buffered bytes remain below the target and that writable capacity can hold the previous nonempty block.
For stable or decreasing decoder block sizes this prevents a predictable partial write and its timed retry.
The previous size is predictive rather than a decoder maximum: if a later block grows, the existing stop-token-aware partial-write loop remains the fallback.
A decoded block larger than the entire ring fails with `DecodeFailed` instead of entering an impossible write wait.

The predictive size is producer-confined.
For an active Engine seek, backend `stop()` first establishes render quiescence and Engine control serialization excludes complete `status()` queue observation.
`StreamingSource::seek()` then stops and joins the decode worker before resetting the byte ring and predictive size, after which synchronous preroll becomes the producer until the worker restarts.
The byte ring reset is constant-time for its trivially destructible element type and requires that no read, write, or queue-availability observation overlap it.
Direct `PcmSource` users must establish the same consumer and observer quiescence before calling seek.
The realtime consumer still performs only ring reads; it does not update a separate occupancy counter, take a lock, or notify the producer.

### Prepared lookahead

Player captures lookahead input, route, current-format, playback-generation, and
start-context evidence on the callback executor. When the current session is
gapless-capable, decoder open, negotiation, and preroll run on an async worker.
Engine adopts the result only after Player's task remains admissible and
Engine's captured playback/route/format context still matches.
If the candidate and current route are gapless-capable, adoption starts the
decode thread, retains the node as lookahead, and arms its raw pointer for
realtime consumption.

When the current session is lossy, unknown, or otherwise not gapless-capable,
preparation produces a logical `DrainFallback` result on the callback executor
without invoking the successor decoder factory. Its application token fixes the
successor identity but is not proof that the successor has opened successfully.

The gapless verdict is fixed at arm time.
The current negotiated format cannot change without consuming or clearing lookahead, so the render thread performs no format read or capability test.

`clearNext` returns the disarmed opaque item id when the render thread has not consumed it.
If already consumed, it returns empty and upper runtime retains matching metadata for the later advanced callback.
Explicit `play`, `stop`, `seek`, and output-device changes clear unconsumed lookahead.
An `updateDevice` call carrying the unchanged device snapshot is a no-op and
does not invalidate pending starts or prepared lookahead.
Prepared-source failure clears lookahead without changing the current track; after splice, that source generation is current and fails as current.

A splice is permitted only when both sessions are gapless-capable and negotiated backend formats are identical.
Current gapless-capable codecs are lossless FLAC, ALAC, and WAV.
Lossy/unknown codecs or format mismatch drain and close; no resampling, channel remapping, or artificial silence forces compatibility.
Lookahead open failure, cancellation, or stale completion leaves the current
session and successor choice unchanged; it neither skips nor redraws a shuffle
candidate.

### Explicit staged start

Track-session construction is split into preparation and activation.
Preparation opens the decoder, reads stream metadata, negotiates and possibly
reopens the decoder, applies the initial seek, and prerolls a `StreamingSource`
without installing an Engine error callback or starting its decode thread.
Activation installs that callback and starts the thread.
The synchronous compatibility path composes both phases.

For `startFromView`, Player captures a move-only preparation value from Engine
and runs preparation on the async worker pool. That value owns copied device,
backend/profile, input, decoder factory, and generation evidence and holds no
Player, Engine, runtime, or frontend reference. Completion resumes on Player's
executor through a stop-token checkpoint and then checks the upper acceptance
predicate. Engine revalidates playback generation, start context, and route
before it allocates source/playback generations, registers the staged source,
and activates it.
Player retires the applicable task handle before invoking acceptance. Acceptance
and completion are outward publications; after acceptance Player rechecks its
callback gate and reacquires the owner, then computes the complete adoption or
error result before invoking completion without later Player access.
When the gate remains open, an acceptance veto completes exactly once with
`Conflict`. The same rule applies to worker lookahead and callback-executor
logical `DrainFallback` lookahead.
An evidence mismatch returns `Conflict`; upper playback owners discard that
stale preparation without presenting it as a media-open or route failure.
If activation or candidate construction fails, adoption removes the staged
registration before returning the error.

The adopted source is still not published to the render timeline.
`commitPlayback` publishes only if the active generation and start context still
match. The old source remains active through worker preparation and adoption;
backend stop/close/open and the transport subject change occur only at commit.

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

PlaybackTransport retains the prepared runtime request by opaque item id.
Natural advance commits matching now-playing metadata without an intermediate idle publication.
When clear returns a disarmed id, only that metadata is removed; metadata for an already spliced item remains until the advanced callback consumes it.

### Runtime publication

PlaybackTransport mutators, state reads, subscription creation, and subscription reset are executor-affine.
Its always-on `ensureOnExecutor` guard logs critically and aborts on violation rather than permitting an unobserved race.
The public `PlaybackService` shares that callback-executor affinity and serializes observer-issued commands through its application commit pump.

Control commands call Player, refresh `PlaybackState`, and emit command-specific signals synchronously.
Asynchronous Player events arrive on a later executor turn.
Lower signal handlers defer owner destruction until publication returns; public playback observers cannot re-enter lower control because their commands execute in a later service turn.

Production hosts supply a real owner-thread executor.
GTK and TUI marshal through their toolkit loops, while CLI drives `LoopExecutor`; a foreign Player callback therefore cannot enter executor-affine service state inline on its producer thread.

### Backend lifetime and properties

Backends protect native handles against public-method/callback interleavings.
They do not hold locks needed by public methods while invoking `RenderTarget` callbacks.

`stop()` is called from the non-render Engine control domain, closes admission of new render cycles, and waits for every admitted cycle to finish before returning.
A render cycle includes `renderPcm` and its directly associated position, underrun, and drain notifications; a backend may deliver such a notification synchronously inside `stop()`, but not after it returns until rendering is restarted.
The target remains open, permitting stop/flush/start seek flows, while non-render route, property, and error callbacks remain protected by generation checks and the `close()` lifetime boundary.
`close()` is the revocation boundary and waits for in-flight target callbacks.
An unrecoverable backend error quiesces its render loop or enters a bounded retry; Engine does not synchronously call stop from the backend error callback.

Volume and mute are Engine runtime state.
A backend accepting properties before stream open caches and reapplies them when the stream becomes live.

### Shutdown

The first `Engine::shutdown()` caller changes lifecycle under the control lock, retires the render session, stops and joins the event worker, and closes backend/timeline state.
Commands admitted after lifecycle transition do not enter backend/timeline logic and result-bearing commands return `InvalidState`.
Concurrent shutdown callers wait for the single teardown; repeated completed shutdown is a no-op.

Player public methods and destruction run on its executor, which outlives Player.
Destruction closes the shared gate and cancels start/lookahead task handles before providers and Engine stop, so already queued tasks return without touching Player state.
Decoder open is not forcibly interruptible; a blocked worker may finish after Player teardown, but after cancellation it can only destroy its own isolated preparation value.
Final runtime teardown stops and joins the worker pool, so the same blocked call
can extend application shutdown until it returns.
`BackendProvider::shutdown()` is `noexcept`; after it returns, provider-owned asynchronous sources cannot initiate new device or graph callbacks.

## Failure and cancellation

External media, device, route, and capability failures cross public audio boundaries as `Result` or asynchronous typed events.
A decoded PCM block larger than the source ring is a recoverable `DecodeFailed` media outcome.
Stale generation events are discarded.
Terminal events remain asynchronous relative to the producer callback, so queries may briefly show the earlier transport.

Engine control has no general stop-token cancellation.
Decoder/source workers and runtime orchestration own their more specific cancellation.
Stop, replacement start, seek, output change, clear, and shutdown invalidate the
applicable Player task handle. The callback-resumption stop-token checkpoint
prevents its late result from reaching acceptance or Engine adoption, but
cancellation does not guarantee immediate return from a decoder or filesystem
call.
Because cancellation, replacement, and teardown end the task path rather than
vetoing acceptance, they may suppress completion entirely.
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
- Audio detail timeline and track-session code under [`lib/audio/detail/`](../../../lib/audio/detail/) owns nodes and decode lifetime; [`StreamingSource`](../../../include/ao/audio/StreamingSource.h), [`PcmRingBuffer`](../../../include/ao/audio/PcmRingBuffer.h), and [`StreamingBufferPolicy`](../../../lib/audio/detail/StreamingBufferPolicy.h) own PCM production, bounded storage, and producer admission.
- [`Player.h`](../../../include/ao/audio/Player.h) and [`Player.cpp`](../../../lib/audio/Player.cpp) own provider composition, executor marshalling, graph epochs, and teardown gate.
- [`Backend.h`](../../../include/ao/audio/Backend.h) and concrete backends under [`lib/audio/backend/`](../../../lib/audio/backend/) own native lifetime.
- [`PlaybackTransport.cpp`](../../../app/runtime/playback/PlaybackTransport.cpp) owns executor-affine transport adaptation and prepared metadata; [`PlaybackService.cpp`](../../../app/runtime/playback/PlaybackService.cpp) publishes the coherent application snapshot.

## Test map

- [`EngineConcurrencyTest.cpp`](../../../test/unit/audio/EngineConcurrencyTest.cpp) protects concurrent commands, status/seek serialization, render/reset exclusion, and teardown.
- [`EngineGaplessTest.cpp`](../../../test/unit/audio/EngineGaplessTest.cpp), [`EngineDrainTest.cpp`](../../../test/unit/audio/EngineDrainTest.cpp), and [`AudioBackendRenderProgressTest.cpp`](../../../test/unit/audio/backend/detail/AudioBackendRenderProgressTest.cpp) protect splice, drain, mixed-buffer progress, and fallback.
- [`EngineCallbackTest.cpp`](../../../test/unit/audio/EngineCallbackTest.cpp), [`EngineErrorTest.cpp`](../../../test/unit/audio/EngineErrorTest.cpp), and [`EngineBackendSwapTest.cpp`](../../../test/unit/audio/EngineBackendSwapTest.cpp) protect generations and stale events.
- [`PlayerTest.cpp`](../../../test/unit/audio/PlayerTest.cpp) protects executor marshalling, graph epochs, and gate behavior.
- [`StreamingSourceTest.cpp`](../../../test/unit/audio/StreamingSourceTest.cpp), [`PcmRingBufferTest.cpp`](../../../test/unit/audio/PcmRingBufferTest.cpp), and [`StreamingBufferPolicyTest.cpp`](../../../test/unit/audio/detail/StreamingBufferPolicyTest.cpp) protect decode-worker lifetime, bounded producer admission, oversized blocks, constant-time reset reuse, and source retirement.
- Runtime playback tests under [`test/unit/runtime/`](../../../test/unit/runtime/) protect executor-affine publication and application metadata.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Playback succession cursor](cursor.md)
- [Decoder session](decoder-session.md)
- [Audio quality analysis](quality-analysis.md)
