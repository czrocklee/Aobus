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
`include/ao/audio/` owns the public Engine, Player, decoder-session, and backend contracts; source-private streaming, concrete decoder, and backend implementations live under `lib/audio/`.
Application runtime consumes Player through executor-affine playback services and never takes backend-specific locks.

## Terminology

- **Control order**: the internal serialization order of concurrent Engine commands.
- **Event worker**: Engine's single non-realtime worker that applies backend/source/render events and invokes Engine callbacks.
- **Render session**: one backend attachment correlated to a render generation.
- **Inspection**: a decoder open with no requested PCM encoding that reports immutable track signal facts without producing playback PCM.
- **Prewarm format hint**: a non-binding, non-blocking Backend prediction used only to choose an optimistic decoder output before native open.
- **PCM mode**: one concrete sample rate, channel count, and `SampleEncoding` used by a backend render target.
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
- The render-to-event ring has exactly two entries; its only legal full state is
  one unconsumed splice followed by that successor's drain completion.
- One serialized producer domain owns all splice and drain-complete pushes for
  an active render target, while `controlMutex` serializes every consumer pop.
- Timeline-node destruction and decode-thread joins happen off the render thread exactly once.
- Preroll and background decode use the same capacity-bounded byte-target policy.
- The source requests another decoded block only while below its buffer target and with predictive block headroom; one decoded block cannot exceed the whole PCM ring.
- Engine callbacks have one origin thread: the event worker.
- Player state and public methods have one owner executor; lower callbacks are marshalled before touching that state.
- A callback from a retired render/audio/provider generation cannot mutate a newer state generation.
- `stop()` establishes render quiescence without revoking the backend's open target.
- `close()` revokes a backend's render target; no render callback begins after it returns.
- Device enumeration does not probe, cache, or carry PCM capability tables.
- A prewarm format hint performs no native open or negotiation and never authorizes a precision reduction, for a track start or for a splice.
- A successful backend `open()` returns the exact PCM mode configured on the native handle used for playback; its client mode and every confirmed endpoint preserve the source precision.
- An idle selected output holds no native playback handle.
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
The synchronous compatibility forms of `stagePlayback` and `setNext` retain that serialization across decoder preparation and token or lookahead publication.
For a compatible lookahead, `setNext` also retains it across final decoder open, preroll, and activation.
Only the explicit asynchronous capture/prepare/adopt forms release serialization while worker preparation is pending and therefore revalidate captured evidence during adoption.

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

The ring capacity is exactly two. Its bound follows from the timeline owner and
backend drain protocols rather than from expected event-worker speed:

1. The timeline owns at most one lookahead node. It is empty, armed, or consumed
   by realtime and awaiting control-side promotion.
2. Lookahead replacement runs under `controlMutex` as one operation: disarm the
   old cursor, wait for any splice handoff, settle every currently visible
   signal, revalidate the captured transition, then arm the replacement.
3. `RenderTimeline::armLookahead()` checks its published cursor and owning slot
   together under the timeline mutex, so a consumed-but-unpromoted owner cannot
   be overwritten even if a future caller bypasses the intended control flow.
4. Consuming the sole armed slot can leave at most one unconsumed `Spliced`
   signal. The successor may then drain and add one ordered `Drained` signal.
5. A backend admits no further render cycle after a drained render result until
   control restarts it, and emits at most one corresponding drain completion.

The legal full sequence is therefore `[Spliced, Drained]`. A third push is a
producer, re-arm, or backend drain-admission defect and aborts through
`AO_RT_INVARIANT`; it is never dropped as a recoverable queue condition.

`kMaxSplicesPerRender == 8` is only a bound on sequential work inside one render
call. Every successful splice consumes the sole armed slot. A later splice in
that same call requires control-side promotion and a new arm, so the sequential
limit is not the simultaneous ring-capacity bound.

The producer side may move between a backend's serialized callback contexts,
but producer entries may not overlap. Engine enforces that exclusion at the
queue boundary. PipeWire does not rely on its main-loop drained callback being
implicitly serialized with the realtime process callback: after a drained
render it closes Aobus render admission, and its main-loop callback posts the
accepted drain completion onto that stream's data loop. Both final
`RenderTarget` calls therefore run on the same data loop. Stop uses a
data-loop barrier, clears pending drain admission, drains the main loop, then
uses a second data-loop barrier so a previously accepted handoff cannot outlive
the target. Every pop and settlement path holds `controlMutex`, so the consumer
side likewise remains singular.

The event worker or next control command promotes the lookahead node, retires the old node, refreshes current format and route/current-input snapshots, then schedules callbacks.
A successful transition emits `onTrackAdvanced`; a drain fallback emits `onTrackEnded` after generation/epoch checks.
If a committed source is already drained with no buffered PCM, or resume or seek
lands in that state before rendering can continue, the control domain performs
the same natural completion directly. It retires the render target, closes the
backend, leaves Engine idle, and queues exactly one `onTrackEnded` for the
accepted playback generation; the backend is never started for an already
drained committed source.

One render call may contain retired-track tail and successor head.
Backends report progress with `RenderPcmResult::positionFrameOffset` and `positionFrames`, not `bytesWritten / frameSize`, so committed tail bytes are not counted as successor position.

A `Drained` completion may become visible after replacement has settled the
ring but before it arms the new lookahead. Ring emptiness is therefore neither
a stable nor a necessary arm precondition: that pending completion has no
unpromoted lookahead owner, and the stopped backend cannot produce another
render signal before control restarts and settles it.

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

Player captures lookahead input, route, current PCM mode, playback generation, and start-context evidence on the callback executor.
When the current session is gapless-capable, decoder inspection runs on an async worker.
If the current PCM mode has the same sample rate and channels and its encoding can preserve the successor's inspected signal, the same worker opens the final successor decoder in that encoding and prerolls it.
Engine adopts the result only after Player's task remains admissible and Engine's captured playback, route, and mode context still matches.
Successful adoption starts the decode thread, retains the node as lookahead, and arms its raw pointer for realtime consumption.

When the current session is lossy, unknown, or otherwise not gapless-capable,
preparation produces a logical `DrainFallback` result on the callback executor
without invoking the successor decoder factory. Its application token fixes the
successor identity but is not proof that the successor has opened successfully.

The gapless verdict is fixed at arm time.
The current physical PCM mode cannot change without consuming or clearing lookahead, so the render thread performs no format read or capability test.

`clearNext` returns the disarmed opaque item id when the render thread has not consumed it.
If already consumed, it returns empty and upper runtime retains matching metadata for the later advanced callback.
Explicit `play`, `stop`, `seek`, and output-device changes clear unconsumed lookahead.
An `updateDevice` call carrying the unchanged device snapshot is a no-op and
does not invalidate pending starts or prepared lookahead.
Prepared-source failure clears lookahead without changing the current track; after splice, that source generation is current and fails as current.

A splice is permitted only when both sessions are gapless-capable and their sample rate, channel count, and concrete encoding match.
The successor's source precision may be lower than the current track's precision because widening it into the already-open mode is lossless.
The successor's precision may never exceed what that mode can represent: an S16 current mode followed by 24-bit audio drains, while a packed-24 current mode followed by 16-bit audio may splice.

Current gapless-capable codecs are lossless FLAC, ALAC, and WAV.
Lossy/unknown codecs or format mismatch drain and close; no resampling, channel remapping, or artificial silence forces compatibility.
Lookahead open failure, cancellation, or stale completion leaves the current
session and successor choice unchanged; it neither skips nor redraws a shuffle
candidate.

### Explicit staged start

Explicit start retains a two-stage acceptance boundary.
Preparation opens an inspection decoder and records source signal facts.
After inspection, Player returns through a stop-token checkpoint to its callback executor and asks Engine to revalidate the captured route before querying `Backend::prewarmFormatHint` under control serialization.
The hint query is non-blocking, performs no native I/O, and may use only immutable policy or cached observations; a missing or invalid hint skips optimistic decoder preparation.
The isolated value then resumes on a worker, opens a final decoder in the valid hinted encoding, applies the initial seek, and prerolls a `StreamingSource` without acquiring a device, changing the active generation, or replacing lookahead.
Failure of this optimistic final-decoder step discards only the prepared source and retains the successful inspection, so it does not become a user-visible failure before commit.
Commit revalidates the token, accepts a new playback generation, retires the old render target, and opens the backend from the inspected `SignalFormat`.
When the returned `clientFormat` exactly matches the optimistic prepared mode, commit activates that prepared source without another decoder open, seek, or preroll.
Otherwise commit discards the optimistic source and synchronously opens, seeks, and prerolls a fresh decoder in the returned `SampleEncoding` before activation.
Source activation installs the Engine error callback and starts the decode thread only after those steps succeed.

For `startFromView`, Player captures a move-only preparation value from Engine and runs inspection on the async worker pool.
The first callback-executor resumption checks cancellation and owner lifetime before the short Engine/Backend hint query; the value then returns to a worker for optional decoder open, seek, and preroll.
Across both worker phases that value owns copied device, backend/profile, input, decoder factory, generation evidence, inspection, and prepared-source values and holds no Player, Engine, runtime, frontend, or Backend reference.
Final completion resumes on Player's executor through another stop-token checkpoint and then checks the upper acceptance predicate.
Engine revalidates playback generation, start context, and route before it returns the staged token.
Player retires the applicable task handle before invoking acceptance. Acceptance
and completion are outward publications; after acceptance Player rechecks its
callback gate and reacquires the owner, then computes the complete adoption or
error result before invoking completion without later Player access.
When the gate remains open, an acceptance veto completes exactly once with
`Conflict`. The same rule applies to worker lookahead and callback-executor
logical `DrainFallback` lookahead.
An evidence mismatch returns `Conflict`; upper playback owners discard that stale preparation without presenting it as a media-open or route failure.
The old source remains active through worker preparation and token adoption; backend stop/close/open and the transport subject change occur only at commit.
Once commit begins replacing the old render target, a backend-open or final-decoder failure closes the newly opened backend and leaves Engine in Error rather than resurrecting the retired source.
That validated attempt has still advanced the audio generation: Player raises its callback floor and resets the route graph, while PlaybackTransport clears prepared requests covered by the new generation and refreshes its Error state.
Engine therefore returns an accepted start receipt for this destructive failure,
with `playbackStarted == false`, rather than returning the same error channel used
for a pre-commit rejection. PlaybackTransport installs the accepted candidate
provenance before the queued typed failure is delivered, but emits neither a
Started event nor a now-playing announcement. A final decoder, seek, or preroll
failure consequently remains a recoverable `TrackOpen` failure for that
candidate; a backend activation or backend-format failure remains a
non-recoverable `RouteActivation` failure.
A destructive commit whose source is already naturally complete also returns an
accepted receipt with `playbackStarted == false`, but it is not a failed start:
Engine remains idle and the queued `TrackEnded` event drives normal succession
without a playback-failure notification.
A pre-commit `Conflict` or `InvalidState` does not advance the generation and therefore preserves the active session and prepared-request registry.

A nonzero staged offset is applied to the final decoder before `StreamingSource` starts its background thread.
An error from a discarded pre-seek epoch therefore cannot invalidate the healthy candidate.

### Identity and callback fences

Engine treats playback item ids as opaque and returns the caller's id in natural-advance events.
`TrackAdvanced`, `PlaybackFailure`, `RouteStatus`, and `TrackEnded` also carry the originating audio generation.

A successful explicit-start, a validated but failed destructive start, or a completed-stop receipt raises the callback-generation floor and synchronizes callback delivery.
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

`Backend::prewarmFormatHint(SignalFormat)` is an advisory control-domain query.
It performs no device open, graph connection, wait, or negotiation; the default predicts the signal's first ordered lossless encoding, while a concrete Backend may use a previously successful same-signal mode or monitor-owned cached evidence.
Unknown or stale evidence is a performance concern only: Engine either skips prewarming or discards a prepared decoder whose complete mode differs from the later `open()` result.

`Backend::open(SignalFormat, RenderTarget&)` requires a live target and performs selection and native activation as one operation.
It returns an `OpenedPcmMode`: the `clientFormat` actually configured, plus an optional `ConfirmedEndpoint` describing the endpoint that mode feeds.
Probe failure and “16-bit only” are therefore not representable as the same cached value, and no open failure causes an implicit 16-bit retry.

The `clientFormat` must preserve the source precision whether or not endpoint evidence exists.
When a `ConfirmedEndpoint` is present, its rate, channels, sample domain, and effective precision must agree with the configured client mode and preserve the source as well.
An absent endpoint means only that the backend could not inspect a direct endpoint; it never weakens the lossless client requirement.

ALSA exposes only direct `hw:C,D` playback endpoints, maps
`Signed24PackedLe`, `Signed24In32Le`, and `Signed32Le` to distinct native
formats, and verifies the applied hardware mode before returning. A plugin PCM
such as `plughw` is not an exclusive/raw endpoint because it may silently
convert sample rate or encoding, so the exclusive backend rejects it.
When the inspected signal has no lossless PCM candidate at all, ALSA returns
`NotSupported` before opening the native PCM handle.

After opening the handle, ALSA fixes access, the exact sample rate, and the channel count, then reads the remaining format mask and the significant-bit count each admissible format resolves.
That snapshot lives only inside the open that produced it; it is not a device capability record and is never consulted to predict a later open.
Selection considers only encodings that preserve the source precision, in the documented lossless order, and requires confirmed significant bits at least as wide as the source.
A missing significant-bit result is unknown evidence rather than full container precision, and a device whose endpoints are all narrower than the source is rejected with the inspected evidence in its error.
An exact rate is required rather than a nearest match, both because the admissible format set depends on it and so an unsupported rate is diagnosed as such instead of surfacing as a format rejection.
A single `snd_pcm_hw_params()` applies the chosen mode, and the applied mode is read back and compared before the handle is published.
Integer and float domain changes outside the documented bit-transparent output set are rejected.

PipeWire, WASAPI, and Core Audio negotiate a client stream in front of a graph that may resample, remix, or requantize without reporting a direct endpoint.
They therefore offer only lossless client encodings and leave `ConfirmedEndpoint` absent, in both shared and exclusive mode.
PipeWire shared mode offers only the first ordered lossless client encoding, while PipeWire exclusive mode retains the complete lossless candidate set and returns the negotiated client stream format.
WASAPI shared mode selects the first ordered lossless client encoding and separately reports endpoint mix format in its graph.
Core Audio shared mode tries the complete ordered lossless encoding set on a
playback-only AUHAL output unit, reads the client stream description back, and
accepts only an exact match. Its graph reports that client PCM stream and the
separately read device-side AUHAL signal format. The latter is downstream route
evidence, not a direct endpoint confirmation. Core Audio device selection uses
the persistent device UID; the transient `AudioDeviceID` is resolved again for
each open, and an explicit selection does not follow later system-default
changes.
Shared backends may convert after accepting that client
stream, but their returned input mode must still be a member of the track's
lossless candidate set rather than an unconditional echo of source metadata.

Selecting or enumerating a device does not reserve it.
Engine creates the render target immediately before `open()` and calls `close()` on every failed or abandoned open path, including failure of the final decoder after native activation.
That interval can briefly acquire and then release an exclusive endpoint when
final decoder setup fails; no committed stream or persistent idle reservation
survives the failure.
An ALSA `EBUSY` open failure is reported as `ResourceBusy`; Engine does not spin, retry blindly, or attempt to preempt another owner.
Playback stops and succession does not skip the track, but the report is transient rather than pinned: the holder can exit at any moment and no event would arrive to retract a permanent notification.

`stop()` is called from the non-render Engine control domain, closes admission of new render cycles, and waits for every admitted cycle to finish before returning.
A render cycle includes `renderPcm` and its directly associated position, underrun, and drain notifications; a backend may deliver such a notification synchronously inside `stop()`, but not after it returns until rendering is restarted.
After `renderPcm` reports `drained`, the backend closes render admission for that
run and publishes at most one `handleDrainComplete()` after the final render
notification. ALSA and WASAPI perform both actions on their single render loop.
PipeWire closes its project-owned admission in the process callback and posts
the native main-loop drained event back to the stream data loop, so the two
Engine-facing producer calls cannot overlap; only an explicit start reopens
admission.
Core Audio stops calling `renderPcm` as soon as the AUHAL callback observes
drain, fills the remainder and later callbacks with silence, and counts a
conservative presentation tail from the device I/O buffer, safety offset,
device and stream latency, plus AudioUnit latency. A non-realtime control worker
closes callback admission, fences the last callback, stops AUHAL, and emits the
single ordered drain completion after that silent tail.
The target remains open, permitting stop/flush/start seek flows, while non-render route, property, and error callbacks remain protected by generation checks and the `close()` lifetime boundary.
`close()` is the revocation boundary and waits for in-flight target callbacks.
An unrecoverable backend error quiesces its render loop or enters a bounded retry; Engine does not synchronously call stop from the backend error callback.

Volume and mute are Engine runtime state.
A backend accepting properties before stream open caches and reapplies them when the stream becomes live.
Core Audio applies both through the per-instance AUHAL linear-gain parameter;
mute writes zero while preserving the cached volume to restore, and graph
evidence classifies the gain as software rather than device hardware control.

### Shutdown

The first `Engine::shutdown()` caller changes lifecycle under the control lock, retires the render session, stops and joins the event worker, and closes backend/timeline state.
Commands admitted after lifecycle transition do not enter backend/timeline logic and result-bearing commands return `InvalidState`.
Concurrent shutdown callers wait for the single teardown; repeated completed shutdown is a no-op.
Engine event-queue destruction requires a non-joinable worker, a published
stopped state, an empty non-realtime event deque, and an empty realtime ring.
Those are always-active invariants in every build configuration.

Player public methods and destruction run on its executor, which outlives Player.
Destruction closes the shared gate and cancels start/lookahead task handles before providers and Engine stop, so already queued tasks return without touching Player state.
Decoder open is not forcibly interruptible; a blocked worker may finish after Player teardown, but after cancellation it can only destroy its own isolated preparation value.
Final runtime teardown stops and joins the worker pool, so the same blocked call
can extend application shutdown until it returns.
`BackendProvider::shutdown()` is `noexcept`; after it returns, provider-owned asynchronous sources cannot initiate new device or graph callbacks.

## Failure and cancellation

External media, device, route, and format failures cross public audio boundaries as `Result` or asynchronous typed events.
Track preparation translates only explicitly typed decoder failures. Allocation,
logic, and other unexpected exceptions are not relabeled as recoverable
`Generic` playback errors: synchronous entry points unwind, while asynchronous
preparation reports the exception through the runtime diagnostic boundary and
does not invoke acceptance or completion.
An inspection failure is a recoverable track-open outcome and does not replace active playback.
An optimistic explicit-start decoder failure is retained only as a cache miss; commit retries decoder setup after the backend returns its exact PCM mode.
A backend activation failure is a non-recoverable route-activation outcome for that start.
A final decoder, seek, or preroll failure after backend activation is a recoverable track-open outcome, but the attempted backend is still closed before publication.
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
- Audio detail timeline and track-session code under [`lib/audio/detail/`](../../../lib/audio/detail/) owns nodes and decode lifetime; source-private [`StreamingSource`](../../../lib/audio/StreamingSource.h), [`PcmRingBuffer`](../../../lib/audio/PcmRingBuffer.h), and [`StreamingBufferPolicy`](../../../lib/audio/detail/StreamingBufferPolicy.h) own PCM production, bounded storage, and producer admission.
- [`Player.h`](../../../include/ao/audio/Player.h) and [`Player.cpp`](../../../lib/audio/Player.cpp) own provider composition, executor marshalling, graph epochs, and teardown gate.
- [`Backend.h`](../../../include/ao/audio/Backend.h), the private [`DecoderOutput.h`](../../../lib/audio/detail/DecoderOutput.h), and concrete backends under [`lib/audio/backend/`](../../../lib/audio/backend/) own advisory prediction, lossless candidate derivation, native selection, and lifetime.
- [`PlaybackTransport.cpp`](../../../app/runtime/playback/PlaybackTransport.cpp) owns executor-affine transport adaptation and prepared metadata; [`PlaybackService.cpp`](../../../app/runtime/playback/PlaybackService.cpp) publishes the coherent application snapshot.

## Test map

- [`EngineConcurrencyTest.cpp`](../../../test/unit/audio/EngineConcurrencyTest.cpp) protects concurrent commands, status/seek serialization, render/reset exclusion, and teardown.
- [`EngineRtSignalRingTest.cpp`](../../../test/unit/audio/EngineRtSignalRingTest.cpp) protects the exact two-entry capacity, serialized producer handoff, sequential-splice occupancy, pending-drain arm behavior, and legal full-ring delivery.
- [`EngineFatalProbeTest.cpp`](../../../test/unit/audio/EngineFatalProbeTest.cpp) and the self-reentering `ao_audio_fatal_probe` under [`test/fatal/`](../../../test/fatal/) protect realtime overflow, timeline-owner, and event-queue destruction fatal invariants in a child process.
- [`EngineTest.cpp`](../../../test/unit/audio/EngineTest.cpp) protects optimistic explicit-start PCM selection, prepared-source reuse, and exact backend-mode fallback.
- [`EngineGaplessTest.cpp`](../../../test/unit/audio/EngineGaplessTest.cpp), [`EngineDrainTest.cpp`](../../../test/unit/audio/EngineDrainTest.cpp), and [`AudioBackendRenderProgressTest.cpp`](../../../test/unit/audio/backend/detail/AudioBackendRenderProgressTest.cpp) protect splice, cross-precision mode reuse, drain, mixed-buffer progress, and fallback.
- [`EngineCallbackTest.cpp`](../../../test/unit/audio/EngineCallbackTest.cpp), [`EngineErrorTest.cpp`](../../../test/unit/audio/EngineErrorTest.cpp), and [`EngineBackendSwapTest.cpp`](../../../test/unit/audio/EngineBackendSwapTest.cpp) protect generations, stale events, typed failures, and synchronous invariant exceptions.
- [`PlayerTest.cpp`](../../../test/unit/audio/PlayerTest.cpp) protects executor marshalling, responsive worker-side preroll, cancellation cleanup, asynchronous diagnostic boundaries, graph epochs, and gate behavior.
- [`AlsaExclusiveBackendTest.cpp`](../../../test/unit/audio/backend/AlsaExclusiveBackendTest.cpp), [`AlsaModeSelectorTest.cpp`](../../../test/unit/audio/backend/detail/AlsaModeSelectorTest.cpp), [`AlsaPcmFormatTest.cpp`](../../../test/unit/audio/backend/detail/AlsaPcmFormatTest.cpp), and [`AlsaPcmErrorTest.cpp`](../../../test/unit/audio/backend/detail/AlsaPcmErrorTest.cpp) protect direct-hardware enforcement, strict lossless selection, significant-bit evidence, exact native format mapping, and open-error classification.
- [`StreamingSourceTest.cpp`](../../../test/unit/audio/StreamingSourceTest.cpp), [`PcmRingBufferTest.cpp`](../../../test/unit/audio/PcmRingBufferTest.cpp), and [`StreamingBufferPolicyTest.cpp`](../../../test/unit/audio/detail/StreamingBufferPolicyTest.cpp) protect decode-worker lifetime, bounded producer admission, oversized blocks, constant-time reset reuse, and source retirement.
- Runtime playback tests under [`test/unit/runtime/`](../../../test/unit/runtime/) protect executor-affine publication and application metadata.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Playback succession cursor](cursor.md)
- [Decoder session](decoder-session.md)
- [Audio quality analysis](quality-analysis.md)
