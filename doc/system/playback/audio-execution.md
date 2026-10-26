---
id: playback.audio-execution
---
# Audio execution and concurrency

## Scope

This specification defines current concurrency and lifetime behavior below the application playback authorities: Engine control serialization, streaming PCM buffering, event-worker delivery, realtime render rules, explicit and gapless transitions, generation correlation, Player executor marshalling, backend responsibilities, and shutdown.

Succession policy belongs to the [playback cursor specification](cursor.md), decoder behavior belongs to the [decoder session specification](decoder-session.md), and cross-layer ownership belongs to the [playback](README.md) and [runtime execution](../execution/README.md) architectures.

## Code boundary

This contract belongs primarily to the **Core libraries** layer in the [system architecture](../overview.md), under the [playback architecture](README.md).
`include/ao/audio/` owns the public Engine, Player, decoder-session, and backend contracts; source-private streaming, concrete decoder, and backend implementations live under `lib/audio/`.
Application runtime consumes Player through executor-affine playback services and never takes backend-specific locks.

## Terminology

- **Control order**: the internal serialization order of concurrent Engine commands.
- **Event worker**: Engine's single non-realtime worker that applies backend/source/render events and invokes Engine callbacks.
- **Render session**: one backend attachment correlated to a render generation.
- **Inspection**: a decoder open with no requested PCM encoding that reports immutable track signal facts without producing playback PCM.
- **Prewarm format hint**: a non-binding, non-blocking Backend prediction used only to choose an optimistic decoder output before native open.
- **PCM mode**: one concrete sample rate, channel count, `SampleEncoding`, and effective speaker mask used by a backend render target.
- **Timeline node**: Engine-owned current or lookahead audio item and its source.
- **Splice signal**: the non-owning realtime notification that a prepared lookahead became active at end of stream.
- **Callback-generation floor**: the minimum audio generation whose materialized callbacks may still begin after a synchronizing receipt.
- **Player graph epoch**: Player's current provider/route graph identity used when accepting marshalled route state.
- **Buffer target**: a requested buffered duration converted to a rounded-up byte count and capped by the PCM ring capacity.
- **Predictive block headroom**: writable ring capacity reserved from the previous nonempty decoded block size before requesting the next block.
- **Render quiescence**: the `Backend::stop()` postcondition that closes render admission and waits for the active render cycle and its render notifications to return.
- **Provider quiescence**: the shared completion reached after subscription admission is closed, monitor and registry producers are retired, and every admitted device or graph callback has returned.

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
- Provider callbacks, subscriptions, and returned backends never require provider-facade storage after the facade is destroyed.
- Concurrent external provider shutdown callers observe one provider-quiescence completion boundary; a callback-thread initiator never waits on its own admitted call.

## State model

Engine owns a lifecycle (`Running`, shutting down, shut down), one serialized control plane, one event queue/worker, backend and route state, a render session/generation, current and optional lookahead timeline nodes, an atomic realtime cursor, a bounded single-producer/single-consumer render-to-event ring, transport/properties, and callback-generation fences.

The render cursor points non-owningly into Engine's control-plane timeline.
The current and lookahead nodes keep source, decoder, and file lifetime alive until a control/event consumer retires them.

Player owns one Engine, all registered providers, route graph state, its executor, and a shared callback/teardown gate.
PlaybackTransport owns runtime metadata separately; `audio::PlaybackInput` contains only path, optional duration, and optional format hints.

## Commands and transitions

### Control and query serialization

Concurrent calls to `play`, `stagePlayback`, `commitPlayback`, `setNext`, `clearNext`, `pause`, `resume`, `stop`, `tryStopWithBarrier`, `seek`, `trySeek`, `setBackend`, `updateDevice`, `setVolume`, and `setMuted` enter the Engine control serialization.
This order guarantees safety and a coherent final state, not user-intent priority between racing callers.
The synchronous compatibility forms of `stagePlayback` and `setNext` retain that serialization across decoder preparation and token or lookahead publication.
For a compatible lookahead, `setNext` also retains it across final decoder open, preroll, and activation.
Only the explicit asynchronous capture/prepare/adopt forms release serialization while worker preparation is pending and therefore revalidate captured evidence during adoption.

The complete `status()` snapshot enters control serialization because it observes the current source's PCM queue.
It waits for an in-flight command such as seek to finish before reading buffered duration, but unlike control-command entry it does not settle pending realtime signals and may still return the pre-splice snapshot.

Scalar state-only queries including transport, backend id, route state, volume, mute, and availability use narrower state or atomic synchronization and remain safe concurrently.
They may observe an intermediate transport until the active control command or event worker publishes its next state.
Backend property observation runs outside Engine's state mutex because a concrete getter may refresh native state and synchronously publish provider graph evidence.
Synchronous backend/provider graph subscribers may call state-only Engine queries such as `volume()`, `isMuted()`, and `isVolumeAvailable()`, including during an initial subscription callback.
Those observations may reflect the command's prior intent because graph publication precedes Engine's scalar-state commit; separate queries are not an aggregate snapshot.
They must not call the complete `status()`, control methods, or `shutdown()`, synchronously destroy Engine, or wait for those operations: graph delivery can hold a provider callback gate or run under Engine control serialization.
Defer that work until after the graph callback returns. Engine's own event-worker callbacks retain their separate control-reentrancy contract below.

Every public control command settles pending splice signals at entry under the control lock.
This closes the window after the realtime cursor changed but before status and current-format state caught up.
Callbacks produced by a control-thread settle are forwarded to the event worker rather than invoked on the caller thread.
The narrower transition-state lock is held only while copying or comparing
current format evidence. Disarming or publishing lookahead and settling
realtime splice signals occur outside that lock because splice settlement also
updates the same transition state.

Pending drain-complete signals are not materialized by command entry.
A command may retire or reposition their render session, so they enter the normal event queue and are rechecked against render generation and drain epoch before notification.

### Seek admission and failure

Once Engine observes `Transport::Error` at control-command admission, `seek()` performs no seek and `trySeek()` returns false, even if the expected item and its source remain present.
This rejection preserves the error text and position and does not disarm lookahead, seek the source, or stop, flush, or restart the backend.
The normal control-entry settlement of already-consumed realtime transitions still precedes admission; rejection is not a rollback of that settlement.

An admitted source seek failure sets Error synchronously and also queues its typed source-error event.
The event worker later retires the failed source and backend through normal failure delivery.
A seek between those two steps must not erase Error with Buffering or restart the source, including when playback was paused before the failure.
A matched `trySeek()` that issued the failing seek still returns true; the failure remains observable through status and the existing asynchronous failure channel.

Recovery establishes a new playback through `play()` or an accepted staged start, including a start chosen by application succession; it is not a same-source seek retry.
The new start clears the previous playback's error text and retains the existing generation fences against retired failure events.
This Engine rule does not retire application occurrences or change Preview semantics; those remain owned by the [application commit specification](application-commit.md).

`tryStopWithBarrier` supports application end requests without substituting a decoder seek.
It rejects Error and a mismatched or zero expected item, disarms lookahead, settles a render-consumed successor, and checks the expected item again before stopping.
A rejected old-item request must not stop the successor whose advance callback is still pending.
An accepted stop returns the same callback/preparation cancellation proof as ordinary stop; Player raises its callback floor and retires covered work only on acceptance.
Transport consumes that proof and emits natural completion rather than the application Stopped event, so succession is not retired and an obsolete queued EOF cannot complete the subject twice.

### Event delivery and reentrancy

Backend events, decoder errors, render transitions, and provider changes enqueue and return.
The event worker applies each event, refreshes state, and invokes callbacks inline afterward.

Engine callbacks may call supported Engine control methods because they are no longer inside the backend/source callback stack.
They must return promptly and cannot call `Engine::shutdown()` or destroy the Engine from the notification; those operations are deferred until the callback returns.

Player repeats this separation at the application boundary.
Engine/provider callbacks queue work onto Player's executor, and Player callbacks to application runtime run from that executor.
Queued work first checks the shared teardown gate.
Player's provider/device caches, route and merged graphs, and derived quality
state belong exclusively to that executor. Public reads and marshalled cache
updates assert the owner boundary. Value copies remain necessary across Engine,
provider, and outward callback calls because those calls can re-enter Player.
Callback replacement and teardown retain their callback mutex and shared gate;
Engine control serialization and provider/backend synchronization remain
independent of these owner-local caches.
Provider graph observations are replaceable latest state. Each active graph
subscription retains at most one pending graph and one delivery chain across the
initial owner hop, Engine event-worker deferral, final owner application, and
outward quality publication. Producers replace the pending payload under a small
mutex; executor, Engine, provider, and user calls occur after releasing that mutex.
A delivery consumes one snapshot. If another graph arrives before outward
publication finishes, the chain schedules a later delivery turn rather than
looping on the owner. Intermediate graphs may therefore be coalesced, but the
latest admitted graph is eventually delivered when producers and consumers make
progress. Retiring a subscription closes graph admission and clears its pending
payload before unsubscribing; its queued application/publication cannot affect a
replacement subscription, even within the same playback generation.

This bounds graph-observation retention by graph size and the active subscription,
not by burst length. Already applied graph and quality snapshots have their normal
bounded delivery lifetimes. Terminal, failure, cancellation, and track-advance
notifications do not use this coalescing path and retain their existing ordering
and generation fences. The general non-realtime Engine deque and other Player
notification paths still have no global capacity bound; graph coalescing does not
establish one or permit dropping ordered events.

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

The PCM ring moves whole output frames only: a truncated write leaves its remainder with the producer at a frame boundary, and a partial-frame read request leaves the fragment buffered.
Ring content is therefore always a whole number of frames, and the realtime consumer yields whole frames to the RenderTarget frame contract by construction.
Track-session preparation rejects an output format without a whole PCM frame as `FormatRejected` before it constructs the source, so no stream reaches the ring with a zero frame size.

Before reading another decoder block, the sole producer checks both that buffered bytes remain below the target and that writable capacity can hold the previous nonempty block.
For stable or decreasing decoder block sizes this prevents a predictable partial write and its timed retry.
The previous size is predictive rather than a decoder maximum.
If a later block grows, synchronous initial or post-seek preroll writes only the available whole-frame capacity and returns without waiting for a consumer.
The source retains the decoder-owned span for the unwritten remainder, and the background producer finishes that block before admitting another decoder read.
Background partial writes remain stop-token-aware.
An end-of-stream block publishes EOF only after all of its PCM has entered the ring, so pending PCM cannot appear drained or suppress worker startup.
A decoded block larger than the entire ring fails with `DecodeFailed` instead of entering an impossible write wait.
A decoded block that is not a whole number of output frames fails the same way instead of parking a tail that can never complete.

The predictive size is producer-confined.
For an active Engine seek, backend `stop()` first establishes render quiescence and Engine control serialization excludes complete `status()` queue observation.
`StreamingSource::seek()` then stops and joins the decode worker before resetting the byte ring, predictive size, and pending block borrow.
The borrow is retired before seeking the decoder; synchronous preroll then becomes the producer until the worker restarts.
The byte ring reset is constant-time for its trivially destructible element type and requires that no read, write, or queue-availability observation overlap it.
Direct `PcmSource` users must establish the same consumer and observer quiescence before calling seek.
The realtime consumer still performs only ring reads; it does not update a separate occupancy counter, take a lock, or notify the producer.

### Prepared lookahead

Player captures lookahead input, route, current PCM mode, playback generation, and start-context evidence on the callback executor.
When the current session is gapless-capable, decoder inspection runs on an async worker.
After inspection, Engine queries the backend's installed-mode reuse contract under control serialization, outside state and transition locks.
If reuse is authorized and the installed encoding and retained confirmed endpoint preserve the successor's inspected signal, a worker opens the final successor decoder in that encoding and prerolls it.
The worker retains only values, not a backend reference.
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
Explicit `play`, `stop`, and output-device changes clear unconsumed lookahead.
A seek clears lookahead only after passing its initial Error and expected-item admission checks.
An `updateDevice` call carrying the unchanged device snapshot is a no-op and
does not invalidate pending starts or prepared lookahead.
Prepared-source failure clears lookahead without changing the current track; after splice, that source generation is current and fails as current.

A splice requires both sessions to be gapless-capable and `Backend::canReuseFormat` to authorize the successor's canonical PCM mode, both before preparation and again at adoption.
The default is exact mode equality, including the speaker mask; this query performs no native I/O, reconfiguration, or outward callback.
An override may additionally prove identical installed transport for stereo masks `0` and FL/FR `0x3`, with the same nonzero rate and concrete encoding.
This does not make zero a wildcard, change decoder identity, or relax ordinary open-result validation.

PipeWire proves the same exact FL/FR offer, installed permutation, and later-format admission for this pair; WASAPI configures the same ascending FL/FR client descriptor.
ALSA additionally requires a healthy installed mode whose current map is exactly unflagged FL,FR in that order; missing, partial, reversed, silent, or flagged observations cannot prove reuse.
The null sink discards either representation identically.
Core Audio keeps the strict default: an initial identity plan does not prove that later layout refresh using the original client mask remains safe for the other source identity.
Its cross-codec `0`/`0x3` transition therefore still drains and reopens when one source remains unidentified.
Default FLAC and WAV effective identities match for counts 1/2/3/4/7/8, so exact canonical mode equality permits reuse even on Core Audio, subject to the same rate, channel geometry, installed encoding, and retained endpoint precision constraints.
Different source interpretation origins do not defeat that equality: these FLAC defaults are `Identified`, while WAV's multichannel compatibility defaults are `Inferred`.
ALAC's [bounded source interpretation](pcm-format.md#alac-source-interpretation) establishes the same canonical mono/stereo identity as ordinary FLAC and WAV when layout absence is trustworthy, enabling strict exact-mode reuse, including on Core Audio, under the same requirements.
Complete supported agreeing explicit ALAC layouts can likewise enable equal-identity reuse after decoder slot normalization; codec names or channel counts alone never guarantee a splice.
ALAC's declared four-, seven-, and eight-channel reference layouts differ from the WAV compatibility sets; their respective `0x107`, `0x13F`, and `0xFF` masks are not replaced with WAV count defaults.

Absent exact mask keys, five/six-channel FLAC reports zero / `Unknown` because back/side geometry is unresolved; otherwise-compatible inferred WAV retains a nonzero effective identity, so that boundary drains and reopens on every current backend.
Complete supported agreeing explicit FLAC back masks `0x37`/`0x3F` can still match WAV's compatibility identities; explicit side masks `0x607`/`0x60F` remain different roles.
A FLAC session with nonfirst STREAMINFO or malformed, conflicting, unsupported, incomplete, or safely uninspectable optional identity evidence also retains zero/`Unknown` under its [source policy](pcm-format.md#flac-source-interpretation).
Untagged ALAC above two channels and ALAC with bad or uninspectable optional declarations retain zero/`Unknown` and legacy ordinal bytes.
Withholding absent-mask five/six-channel FLAC identity can permit otherwise-compatible zero/zero reuse with untagged multichannel ALAC or unknown FLAC, unlike the earlier count-derived `Identified` back-mask interpretation.
Zero/zero reuse proves neither track's original speaker intent nor native routing; ordinal/default transport can still misroute content.

Relative to the earlier all-zero FLAC producer, same-codec FLAC↔FLAC known↔`Unknown` mono and 3–8-channel pairs, or pairs with different nonzero role sets, lose otherwise-equal splice eligibility in both directions.
The same identity boundary conditionally loses FLAC↔ALAC eligibility relative to their earlier all-zero producers: identified FLAC versus unknown ALAC, or unknown FLAC versus identified ALAC, drains and reopens for mono and multichannel modes.
Stereo known FL/FR `0x3` versus zero has only the bounded backend exceptions above; Core Audio remains strict, ALSA requires its healthy installed canonical unflagged FL,FR proof, and PipeWire, WASAPI, and null retain their opt-ins.
Other different stereo role sets do not acquire an alias.
Equal known masks and equal zero/zero masks remain eligible under the ordinary geometry, encoding, and retained-endpoint checks.
Forgiving optional tags admits more tracks without restoring every clean-track↔unknown-track splice; uncertainty never authorizes a mask wildcard or bypasses reuse checks.
These are conditional gapless-eligibility differences, not universal playback failures or a universal WAV↔FLAC/ALAC splice promise.
Explicitly different role sets still drain and reopen, and a fresh native open may refuse an incompatible route; public source facts are never rewritten by backend observations.

Preparation and adoption both validate the successor's own canonical mode against the confirmed endpoint retained for this native open; promotion refreshes source interpretation without replacing that endpoint bound.
Native mapping evidence remains tied to this open and its actual observations, so it may stay more conservative than a fresh successor open would report.
Retained native evidence never identifies an unknown source or manufactures a new observation.
The successor's source precision may be lower than the current track's precision because widening it into the already-open mode is lossless.
The successor's precision may never exceed what that mode can represent: an S16 current mode followed by 24-bit audio drains, while a packed-24 current mode followed by 16-bit audio may splice.

Current gapless-capable codecs are lossless FLAC, ALAC, and WAV.
Lossy/unknown codecs or format mismatch drain and close; no resampling, channel remapping, or artificial silence forces compatibility.
Consecutive segments use this same gate and one-lookahead arming contract.
They splice without a gap when the successor is armed before the current segment ends; a segment shorter than callback/preparation latency can drain before its successor is armed.
There is no callback-independent continuous-carrier source or unconditional gapless promise for arbitrarily short segments.
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
candidate. A backend's explicit track/selected-route incompatibility has the
same classification; an output fault or malformed successful backend mode
remains a non-recoverable `RouteActivation` failure. Error codes or diagnostic
text alone do not decide whether succession may skip the candidate.
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
They do not hold native-handle locks needed by public methods while invoking `RenderTarget` callbacks.

A `BackendProvider` facade initiates discovery and graph shutdown but is not the lifetime anchor for asynchronous provider work.
Concrete providers keep only narrow shared control, monitor, device-registry, graph-registry, and publication state where an independently retained worker, subscription, callback stack, or Backend requires it.
Device and graph subscriptions may reset after facade destruction and may race a copied publication snapshot.
A Backend returned before provider destruction may later close or publish properties safely; provider retirement has already made its publication route inert.
Neither handle keeps the provider facade or its complete PImpl alive.

Subscription registration is concurrency-safe and linearizes its initial snapshot with monitor publication.
A callback may destroy the provider, including during that initial synchronous snapshot; provider code pins independent control state through return and performs no later facade access.
Once shutdown starts, new subscription attempts return inert handles without an initial callback.
Retiring registries during an externally initiated shutdown may synchronously deliver their already-accepted terminal snapshots, and those callbacks count toward the same completion boundary.
When shutdown begins from inside a provider callback, nested teardown publication for that provider is not admitted, preventing recursive facade destruction.
After an external `shutdown()` returns, no provider device or graph callback can begin or remain active.

`Backend::prewarmFormatHint(SignalFormat)` is an advisory control-domain query.
It performs no device open, graph connection, wait, or negotiation; the default predicts the signal's first ordered lossless encoding, while a concrete Backend may use a previously successful same-signal mode or monitor-owned cached evidence.
A known source-only refusal yields no optimistic hint; concrete backends apply the same bounded admission rule in hint and open without performing native negotiation in the hint.
Unknown or stale native evidence is a performance concern only: Engine either skips prewarming or discards a prepared decoder whose complete mode differs from the later `open()` result.

`Backend::open(SignalFormat, RenderTarget&)` requires a live target and performs selection and native activation as one operation.
It returns an `OpenedPcmMode`: the `clientFormat` actually configured, plus an optional `ConfirmedEndpoint` describing the endpoint that mode feeds.
Probe failure and “16-bit only” are therefore not representable as the same cached value, and no open failure causes an implicit 16-bit retry.
A successful open is configured-mode evidence for that attempt, not a liveness lease: a permitted observer-driven close under [backend graph delivery](#backend-graph-delivery) can retire the target before open returns, and a raw backend caller must itself coordinate the observer controls it allows.

The synchronous `BackendOpenResult` carries either that mode or a
`BackendOpenFailure` containing the original `Error` and a causal classification.
`TrackRouteIncompatible` identifies a source or selected-route constraint another
track may satisfy: an empty lossless source-encoding set, an intentionally
rejected rate/count, or confirmed insufficient endpoint precision.
`OutputFailure` is the conservative default for busy, lost, uninitialized or
broken outputs, unknown native causes, failed readback and post-admission
inconsistency. A generic `FormatRejected` or `NotSupported` code does not imply
either class. Engine preserves the original error and translates only explicit
incompatibility into recoverable `TrackOpen`; output faults remain terminal
`RouteActivation`. A malformed successful mode is still an output fault.

Public backend entry checks remain necessary for direct callers, including
unrepresentable source precision. Inspection rejects zero rate, channel count and
precision before normal playback reaches a backend. At the direct backend boundary,
zero rate/count is invalid geometry and remains conservative `OutputFailure`;
an empty lossless encoding set, including zero source precision, is an explicit
`TrackRouteIncompatible` refusal. Neither decision is inferred from the `Error` code.
Deterministic source refusal precedes that backend's own close/native configuration where possible. Engine already closes
its old stream before invoking open, so these guards do not retain or resurrect
Engine's previous playback. Application succession keeps its existing
three-consecutive-failure limit and resets it only on a real successful start;
initial staging, prepared-token and explicit-navigation ownership is unchanged.

The `clientFormat` must preserve the source precision whether or not endpoint evidence exists.
When a `ConfirmedEndpoint` is present, its rate, channels, sample domain, and effective precision must agree with the configured client mode and preserve the source as well.
An absent endpoint means only that the backend could not inspect a direct endpoint; it never weakens the lossless client requirement.

#### Channel compatibility and native evidence

WAV's [source interpretation](pcm-format.md#wav-source-interpretation), FLAC's distinct [source interpretation](pcm-format.md#flac-source-interpretation), and ALAC's [bounded metadata policy](pcm-format.md#alac-source-interpretation) activate canonical PCM with effective speaker identities without a blanket multichannel ban.
WAV's `Identified` or `Inferred` history and FLAC's and ALAC's `Identified` histories do not establish native mapping or physical routing.
FLAC with absent exact mask keys at five/six channels, nonfirst STREAMINFO, or malformed, conflicting, unsupported, incomplete, or safely uninspectable optional identity evidence retains zero masks and `Unknown` rather than a guessed identity.
ALAC above two channels without a declaration, or at any count with bad or uninspectable optional metadata, likewise retains zero/`Unknown`, not a new source-open refusal; mandatory container/configuration and precision errors remain unchanged.
That best-effort playback admission is not a role-safety or native-routing guarantee: zero-mask paths can use ordinal/default transport through different speaker roles, and native open can still fail.
Numerical bit-perfectness is independent of channel confidence.
The other current codec producers, AAC, Opus, and MP3, retain zero masks and `Unknown` source interpretation; neither WAV compatibility sets nor FLAC defaults are ALAC or backend defaults.
ALAC normalization changes no backend admission, native observation, or quality-analysis policy.

Source admission and native delivery evidence are separate decisions.
A backend may admit transport without proving every speaker relationship; the returned canonical client mask must still retain the source identity.
Requests, accepted native configuration, and current observations have the distinct authority defined by each backend below; none manufactures source identity from a zero mask.
Private native slot permutations do not change canonical PCM identity.

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
A missing significant-bit result is unknown evidence rather than full container precision and cannot establish a skippable precision incompatibility. When every relevant admitted candidate has confirmed insufficient precision, refusal is `TrackRouteIncompatible`; an unresolved relevant candidate keeps refusal conservative `OutputFailure`. The inspected evidence remains in the error.
An exact rate is required rather than a nearest match, both because the admissible format set depends on it and so an unsupported rate is diagnosed as such instead of surfacing as a format rejection.
A single `snd_pcm_hw_params()` applies the chosen mode, and the applied mode is read back and compared before the handle is published.
Integer and float domain changes outside the documented bit-transparent output set are rejected.

Only after required applied storage and precision succeed, ALSA independently queries offered and current channel maps.
A NULL result withholds that observation, not storage eligibility; it does not excuse an invalid source declaration. Every nonzero source mask must contain exactly one assigned WAVE position per channel, regardless of current-map availability, or admission is `TrackRouteIncompatible` after required native validation.
All offered maps face bounded list and storage checks, even when their channel counts differ from this mode. Only matching-count offers undergo role-semantic validation and can veto admission; their positions never select the current slot order, and the backend never sets a channel map.
Malformed successful current observations remain output failures, including for a zero-mask source. These independent native failures are not converted into layout refusals or unknown evidence.
A bounded compatibility analysis recognizes bare repeated `SL`/`SR` in ordered subsets of the old Linux UAC1 conversion table before its [surround-role correction](https://github.com/torvalds/linux/commit/b7b7e1ab7619deb3b299b5e5c619c3e6f183a12d): `FL, FR, FC, LFE, SL, SR, FLC, FRC, RC, SL, SR, TC`.
It includes truncated selections, absent-configuration prefixes and trailing `UNKNOWN` padding within that kernel table's fifteen-slot storage bound, but not flags, other duplicate identities, interior padding or reordered signatures.
Nonduplicate maps retain ordinary ALSA role semantics; this signature neither establishes a device's origin nor supplies a guessed rear/side assignment.
One classification validates offers and current observations; a recognized offer never selects slots or overrides current evidence.
A matching-count offer remains an availability fact, including partial or ambiguous roles, not proof of complete or current mapping.
Raw observations are retained; a separate admission view withholds only occurrences that could be the old table's erroneous early surround pair, preserving later known side roles.
For example, `FL, FR, FC, LFE, SL, SR, SL, SR` becomes `FL, FR, FC, LFE, UNKNOWN, UNKNOWN, SL, SR` for the existing partial-position check: complete source mask `0x63F` remains eligible without permutation or mapping proof, while `0xFF` still conflicts at the known side slots.
Zero-mask sources retain identity transport; accepted ambiguous maps supply no native speaker mask or mapping proof.
Independent native/storage faults and invalid source declarations retain their precedence over this compatibility decision.
For a known source, other complete matching current positions establish a private immutable slot permutation; partial compatible positions remain uncertain and known contradictions are explicit track/route incompatibilities.
A partial current map is compatible only when each known role sits in the unchanged canonical slot the source's ascending assigned-bit order gives it, because partial evidence cannot prove a permutation: for a stereo `FL`/`FR` source, `[FL, UNKNOWN]` and `[UNKNOWN, FR]` remain uncertain, while `[FR, UNKNOWN]` is a known contradiction refused as a recoverable `TrackRouteIncompatible`.
A zero-mask source retains legacy no-permutation eligibility and never acquires source identity from a known endpoint. Current positions may still populate endpoint and graph evidence; they do not prove the unknown source's mapping.
The playback loop checks tightly interleaved MMAP geometry and uses preallocated frame scratch for any permutation before gain and commit.
The graph reports native storage without a canonical input mask and carries observed native positions separately.

#### Core Audio channel admission

Core Audio admits zero-mask legacy sources and complete supported WAVE speaker sets without deriving roles from count.
Direct open checks geometry, then lossless encoding/precision, then local identity completeness before its own close or native access; precision wins over layout for composite-invalid input.
Known-mask prewarm preserves rate, count and identity using only lossless local policy, not native observation or an admission guarantee.
After initialization, a fresh exact input ASBD must still match the selected storage.
The canonical opened client mode and initial delivery snapshot are returned even when notification reentry retires that open; AUHAL shared output never supplies a `ConfirmedEndpoint`.

The bounded bridge independently reads actual AU client/output layouts and destination-to-source output ordinals (`-1` is silence).
Current AU output layout is authoritative; device preference is only an applicable-count fallback, never proof of current output geometry.
Native tags and bitmaps expand through AudioFormat, with exact byte extent, description/count and map-range bounds.
Only documented AU property-absence statuses withhold actual layout/map evidence; native faults and malformed successful required reads remain output failures.
Ordinary unusable optional preferences withhold evidence, but queried device loss is terminal.
Explicit native labels distinguish back from side; opaque Mono never implies Center, and coordinate positions are not proved by this vocabulary.
A local output-unit map may be set only after incompatibility and independently complete equal-set observations; explicit map SET capability refusals are contextual incompatibilities, while successful SET requires fresh matching readback and subsequent role proof.
All accepted paths remain `isMapped == false`: no physical-route or mono-gain guarantee is supplied.

Before render admission, Core Audio provisions immutable canonical pull geometry, source slots, sample/frame sizes, frame capacity and buffers.
Identity binds validated native storage directly with a preallocated null-storage fallback; only nonidentity allocates a second buffer.
Permutation copies every sample-storage byte without numerical conversion.
One interleaved buffer, exact client count, capacity and UInt32 byte bounds are checked before binding mutation.
No realtime allocation, property read, layout parsing, map change or plan replacement occurs; the full callback fence covers render and associated position, underrun and drain work.
Observed native sets do not imply canonical native byte order; the graph reports native storage, with client-to-output channel conversion only on the receiving Sink, not a duplicate Stream finding.

Start/resume synchronously prove fresh exact input storage and unchanged provisioned slots under target serialization then the control-only run gate.
Execution omits dispensable output numerics, preference and latency work when current layouts/maps suffice, and queues optional worker observation afterward.
Observation clears unavailable downstream numerics instead of borrowing client, preference or cached geometry; missing current output geometry or a changed required permutation faults rather than remapping active buffers.
A queried downstream ASBD must have a finite positive rate and nonzero channel count, independently of the optional `SignalFormat` projection's integer-rate and eight-bit-count limits.
Unrepresentable downstream numerics withhold diagnostic signal evidence without rejecting the proved client transport; this does not validate downstream sample storage or add client encodings.
Exact successful ASBD byte extents, fresh client storage and native device-loss failures remain mandatory checks.
The worker holds native ownership through getters, relationship validation and snapshot assignment so an older read cannot publish after a newer one.
Retired/closed refreshes discard both success and failure. Tail-latency replacement is non-realtime serialized native work.
Listeners only enter/leave their fence and signal pending work; partial registrations are tracked and listener admission closes before unregister/wait/disposal.

`CoreAudioControlMailbox` arbitrates pending listener/render generations monotonically and retains the first pending fault cause per generation.
That coalescing does not order already consumed actions: `CoreAudioRunControl` separately orders worker refresh/fault/drain native work against public run advancement.
Realtime/listener generation reads remain lock-free; tokens do not retain owner lifetime.
Workers release run/native/snapshot locks before target callbacks or graph delivery and never hold the run gate while waiting for target serialization.
The [final close and unlocked delivery ordering](#backend-graph-delivery) remains the target lifetime authority; worker handoff alone does not prove another callback has returned.

#### Shared client formats

PipeWire, WASAPI, and Core Audio negotiate a client stream in front of a graph that may resample, remix, or requantize without reporting a direct endpoint.
They therefore offer only lossless client encodings and leave `ConfirmedEndpoint` absent, in both shared and exclusive mode.
PipeWire shared mode offers only the first ordered lossless client encoding, while PipeWire exclusive mode retains the complete lossless candidate set and returns the negotiated client stream format.
WASAPI shared mode selects the first ordered lossless client encoding and separately reports endpoint mix format in its graph.
Only `IAudioClient::Initialize`'s explicit `AUDCLNT_E_UNSUPPORTED_FORMAT` rejection
of that offered client format is `TrackRouteIncompatible`; this is not evidence
that every lossless encoding was attempted. Other initialization failures remain
`OutputFailure`, including device, service and resource faults. Native return
values are classified in the operation that produced them, not by a global
error-number policy shared with readback or post-admission failures.
Core Audio shared mode tries the complete ordered lossless encoding set on a
playback-only AUHAL output unit, reads the client stream description back, and
accepts only an exact match. Only an explicitly classified format-set rejection
advances to another candidate; failed readback or a mismatched successful
readback is an output failure, not evidence that every candidate is unsupported. Its graph reports that client PCM stream and the
separately read device-side AUHAL signal format. The latter is downstream route
evidence, not a direct endpoint confirmation. Core Audio device selection uses
the persistent device UID; the transient `AudioDeviceID` is resolved again for
each open, and an explicit selection does not follow later system-default
changes.
Shared backends may convert after accepting that client
stream, but their returned input mode must still be a member of the track's
lossless candidate set rather than an unconditional echo of source metadata.

#### PipeWire client channel contract

PipeWire channel admission uses the Format param actually read back from the negotiated client stream.
The offer is not that observation, and retaining the request never establishes mapping evidence.
Only the two-channel cohort offers positions.
A known two-channel source offers its declared WAVE role set, in ascending assigned-bit order.
Mask `0x30` therefore offers RL then RR, not FL/FR.
A zero mask offers FL/FR.
The returned client mask stays the source mask, so a zero client mask never becomes that offered identity.
Mono and multichannel offers are unpositioned.
An unpositioned offer imposes no role prediction; the negotiated observation may still report known positions.

Required storage, shape, numeric, and precision faults are terminal `OutputFailure`.
That includes an unrecognized or malformed position table, a position count that does not match the negotiated channel count, and a successful mode outside the lossless offer.
These faults are never reclassified as an unknown layout or as `TrackRouteIncompatible`.
A successful two-channel readback whose native role set is not exactly that offered set is the same terminal output failure, including unsupported, incomplete, or different positions.
FL/FR is the fallback for a zero source mask; a known source is compared with its declared two-role set.
A reordering of that same offered set, including FR then FL or RR then RL, matches the set and is accepted as a private permutation.

`UNKNOWN`, `MONO`, and generic AUX positions leave WAVE identity unknown.
SPA NA is an explicit silent slot, not `UNKNOWN` and not a speaker identity.
CUSTOM ids and other non-WAVE spatial roles are unsupported role constraints, not malformed storage and not guessed WAVE speakers.
Repeated `UNKNOWN` and repeated NA are permitted position-table shape, not duplicate identity.
A silent NA remains unsupported whether it appears once or is repeated.
Strict client parsing accepts that shape.
It still rejects duplicate actual WAVE, AUX, or CUSTOM roles, reserved gaps, and invalid position ids as malformed storage.
Monitor observations keep their separate best-effort rule: semantic unusable roles preserve numerics and known subsets, while a malformed or non-fixed position property invalidates the observation.
Repeated NA remains an unsupported silent constraint there as well.

An identified source must declare a complete supported WAVE speaker set before native negotiation.
Observed WAVE roles must be a subset of that declaration, and the observation must not carry an unsupported role constraint.
Slots that leave WAVE identity unknown may remain.
Compatible evidence may therefore contain no WAVE roles or only a partial subset of the declared roles.
Those admissions do not set `isMapped`.
On first admission, a known contradiction on this mono or multichannel path is `TrackRouteIncompatible`.
The cause is explicit; it is never inferred from the `Error` code.
An identified mono or multichannel observation of silent NA, CUSTOM, or another unsupported constraint is that contradiction.
The same constraint in a two-channel observation fails that exact offered set as `OutputFailure`, whether the offer is the zero-mask FL/FR pair or another declared pair.
ALSA's rule that partial evidence cannot prove a permutation remains in the ALSA current-map contract above and does not govern this transport.

A zero-mask non-stereo source is admitted when required storage is valid and the observation keeps the same rate, channel count, and encoding.
Its transport is identity: samples are not permuted, and the source does not acquire the observed roles.
Unsupported constraints on that path remain uncertainty rather than a refusal.
Exact two-channel policy is unchanged, so unsupported NA in a two-channel observation still fails that exact offered set: FL/FR for a zero mask, and the declared two-role set for a known source.

When admitted positions and a declared source mask both exist, PipeWire may install a private slot permutation.
A partial permutation is a deterministic lossless bijection: every source slot is used once, and every destination receives one source slot.
Each native destination with a known role receives that role's canonical source slot.
Unused source indices complete the remaining destinations.
That assignment does not put the native destination into canonical order.
That completion does not assign identities to unknown native positions, does not prove a full role permutation, and does not set `isMapped`.
The installed plan is what the render path applies before those frames are queued.
The returned snapshot does not carry the slot table.

`isMapped` is true only when the actual native observation is a complete unambiguous WAVE speaker set, with no unsupported constraint.
It is independent of source interpretation history, so a zero-mask source can still be mapped when that native set is complete.
A zero-mask source has no complete source identity, so channel confidence stays `Unknown` even when a native boundary has `isMapped` true.
Native mapping evidence does not identify that source.
The [quality analysis contract](quality-analysis.md#result-axes-and-confidence) owns those grades.
The native mask is a position set.
It does not assert canonical native byte order or physical wiring.
Current route-graph evidence recomputes the flag from the monitor's current Format cache.
A later observation that is no longer complete clears it; the flag is not a sticky lease.

`OpenedPcmMode::channelDelivery` is the initial snapshot projected during `open()` from the latest accepted observation before `open()` returns.
The installed render delivery is fixed at first admission and is never mutated by a later Format param.
A later parsed zero-mask non-stereo observation with the same rate, channel count, and encoding may refresh only the control-side native cache.
That cache is the comparison baseline for the next Format param.
It does not change installed transport, and it does not mutate the mode already returned by `open()`.
The open snapshot includes such a refresh only when that observation arrives before `open()` returns, and that projection may then differ from the first admitted render plan.
After an observation has been accepted, any later observation that is not one of those permitted refreshes is `OutputFailure`, including a storage change and a role change on an exact two-channel or identified path.
That later fault is not reclassified as a recoverable layout contradiction merely because the new roles would have been one in isolation.
Current graph channel evidence comes from the monitor's independent Format cache when that graph is refreshed.
It is not the open return value and not a delivery of the stream Format callback.
Stream and monitor observations have no one-to-one arrival or scheduling guarantee.

After admission, a terminal Format or stream fault revokes render admission and is reported to the target.
`start()` must not restore that admission while the fault remains.
Only a new `open()` clears it.
The failure remains an output fault under the classification above.

#### Activation, settlement, and controls

Selecting or enumerating a device does not reserve it.
Engine creates the render target immediately before `open()` and calls `close()` on every failed or abandoned open path, including failure of the final decoder after native activation.
That interval can briefly acquire and then release an exclusive endpoint when
final decoder setup fails; no committed stream or persistent idle reservation
survives the failure.
An ALSA `EBUSY` open failure is reported as `ResourceBusy`; Engine does not spin, retry blindly, or attempt to preempt another owner.
Playback stops and succession does not skip the track, but the report is transient rather than pinned: the holder can exit at any moment and no event would arrive to retract a permanent notification.

`stop()` is called from the non-render Engine control domain, closes admission of new render cycles, and waits for every admitted cycle to finish before returning.
"Non-render" names Engine's control domain; the notification-origin run `start()`/`stop()` support below is Core Audio specific and does not imply the same reentry support for other backends.
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
closes callback admission, fences the last callback, and stops AUHAL before
emitting the single ordered drain completion after that silent tail.
A native stop failure withholds drain completion and is reported after releasing native and run-control locks.
Core Audio records a successful native start until a native stop succeeds or the unit is detached; a failed stop does not fabricate a stopped unit or independently prove that the unit is still running.
Pause closes and settles Aobus-owned render admission without advancing the run generation, then requests a native stop when a successful start is still recorded.
While that stop remains unconfirmed, Aobus-owned render admission stays closed, start/resume cannot reopen it, and flush cannot reset the unit.
Each later explicit stop request, including flush's stop phase, may make one native stop attempt; Core Audio control-worker error settlement does not retry an already failed stop.
Engine seek issues stop followed by flush: a confirmed first stop makes flush's stop phase an idle no-op, while a failed first stop permits another native attempt for that distinct request.
Native stop diagnostics preserve the raw status and use the existing per-run error-delivery gate: only the first admitted error for the current generation is delivered.
A withheld resume after failed pause therefore emits no duplicate error for that generation; a previously delivered fault can likewise suppress the later pause diagnostic without clearing its saved stop status.
Distinct stop, flush-stop, and withheld-start generations during a failed seek can each report an error for the same attached target; the exact callback count depends on interleaving.
Engine's target generation is separate: it settles one terminal playback failure and discards subsequent stale events rather than treating each backend run error as a new playback failure.
A successful later stop repairs native activation/reset eligibility, but does not revive a logical run deactivated by an error: same-run resume remains inactive, while a new start establishes a new run after fresh execution proof.
It does not retract an error already delivered to Engine. In particular, a current-target seek stop error remains terminal even if flush's distinct stop attempt succeeds; explicit track replacement instead retires the old Engine target before teardown stop, so that old target's stop error cannot fail the replacement.
Unit detachment ends the run-status record for that unit; native cleanup and target revocation remain owned by close.
After detachment and target revocation, close directly requests native stop, uninitialize, and disposal as best-effort cleanup, outside the injected run-control bindings.
Those cleanup statuses are ignored and are not evidence that the native stop or disposal succeeded.
The target remains open, permitting stop/flush/start seek flows, while non-render route, property, and error callbacks remain protected by generation checks and the `close()` lifetime boundary.
`close()` is the revocation boundary and waits for in-flight target callbacks.
An unrecoverable backend error quiesces its render loop or enters a bounded retry; Engine does not synchronously call stop from the backend error callback.

Volume and application mute are Engine runtime intent, including when a backend reports that a valid requested write failed.
Backend mute properties expose application intent, not an observed effective route mute, so an external hardware switch cannot become Engine or persisted application mute.
Engine captures backend property values and Volume metadata without its state mutex and then applies that observation under the mutex.
The common Backend API still exposes separate scalar reads and does not promise that every platform supplies an atomic multi-property bundle; ALSA's pure application-mute read means this synchronization performs only one native mixer refresh.
After each valid volume or mute request returns from the backend, Engine refreshes the backend's Volume metadata and commits the requested intent, availability, and hardware-assisted state together under its state mutex.
That metadata refresh does not read back the backend volume, so a software-unity fallback cannot replace the valid request.
Explicit Engine stop resets playback state while preserving its existing volume and application-mute intent and refreshing Volume capability from the closed backend.
It does not replace that intent with the closed backend's scalar fallback or issue another control write.
Construction, successful open, and backend replacement retain their separate backend-observation semantics.
A NaN volume request is rejected before any backend control or metadata query and does not mutate Engine intent or capability state; finite values and infinities retain their backend-specific semantics.
PipeWire, WASAPI, and Core Audio cache application controls accepted before stream open and apply them when the stream becomes live.
Core Audio applies both through the per-instance AUHAL linear-gain parameter;
mute writes zero while preserving the cached volume to restore, and graph
evidence classifies the gain as software rather than device hardware control.
ALSA handles shared mixer state under the separate contract below.

### Backend graph delivery

The shared `BackendGraphRegistry`, consumed by ALSA, WASAPI, and Core Audio, suppresses publication when the complete route graph equals the stored graph.
Ordinary publish, clear, and initial subscription delivery do not nest graph callbacks, including when a callback reads a backend property that publishes again.
Reentrant work waits until the active callback returns; revision checks discard superseded publications and cancellation is checked immediately before each callback.
A normal subscription delivers its captured initial snapshot synchronously; a subscription made inside a graph callback defers that initial delivery and can be cancelled before it runs.
A clear still removes the stored graph and publishes an empty snapshot rather than storing an empty graph in place of the route fallback.
An explicit clear can therefore deliver an empty withdrawal to live subscriptions even when no nonempty graph from that route ever reached an observer, for example a clear racing the first publication; that delivery still passes the revision and cancellation checks.

Graph preparation commits the snapshot and its revision without waiting for the callback gate or invoking observers.
Its move-only delivery ticket is one-shot; dropping it cancels delivery without undoing the committed snapshot.
Delayed delivery still checks route revision, subscription cancellation, and registry retirement.
Core Audio prepares publications and clears under its owner publication lock, then releases that lock before delivering.
Run controls, drain and fault transitions, and non-render target notifications share callback serialization.
Core Audio render admission binds each admitted cycle to an immutable run generation before its geometry checks, rendering, or drain work.
The admission generation is published before opening the fence and replaced only while admission is closed and all admitted cycles have settled; capturing it requires no realtime lock or allocation.
The shared callback fence increments its in-flight count only when an open-state compare/exchange admits the callback; rejection from a closed fence never changes the count.
Thus continuing rejected native callbacks cannot manufacture an in-flight borrow between quiescence and reopening, including after an unsuccessful native stop.
All render-origin faults and drain signals carry that captured generation rather than sampling the current control generation when they publish.
A callback admitted between stop's generation advance and fence closure therefore still belongs to the retiring render interval and cannot rebrand its fault as the stopped generation's current error.
Pause does not advance the generation, so resume preserves the same run identity and genuine same-run faults remain eligible.
Native control and device-listener failures retain their separate explicit generation checks; this does not make stop a universal error-callback revocation boundary or retract an error already delivered before retirement.
Start retains that serialization through render admission and native activation, including when called by a graph observer without an enclosing target notification.
Close closes listener admission, then acquires the same serialization before retiring the target attachment and run, disabling render admission, and waiting for final render quiescence.
That final fence follows admitted non-render notifications and run controls: an open-origin notification can reenter start while close waits for it.
Only after this fence may close detach native state, clear render storage, or revoke the target.
Callback serialization is released before publication locking; the lifetime guarantee is close completion, not an entry-time cutoff of old-generation activity.
Open-origin route-ready and format notifications use the same callback serialization as non-render control notifications, so an external close waits for an admitted notification to return.
Open checks its captured target-attachment generation and target identity before admitting route-ready and enabling publication, then rechecks before the format notification and after it returns.
Run-only start or stop does not retire that attachment: the same target still receives its negotiated format and initial graph publication.
Close or replacement retires the notification sequence, which then initiates no graph publication; ordinary publication reads current state rather than replaying an obsolete open's captured graph.
A target callback must defer close and target replacement on its invoking backend until it returns, as required by the [Backend contract](../../../include/ao/audio/Backend.h); close has no self-callback lifetime exception.
This includes a graph callback nested beneath that same backend's target callback.
Graph-observer close remains supported when no callback to that backend's target remains on the same call stack.
A target callback must not wait for observer-driven close or synchronously destroy its own target or backend; the current callback's borrow must remain alive through return.
Core Audio target callbacks have an additional shared-gate precondition: they must defer operations that may acquire or wait for the same registry's delivery gate until the target callback returns, and must not wait for that deferred work.
This includes subscription, subscription reset or destruction, ordinary publication or clear, pending-ticket delivery, registry retirement, and backend/provider operations that reach the gate indirectly.
A different route anchor or backend identity does not avoid this precondition when it uses the same registry.
An ordinary graph observer may already hold the gate while waiting for a run control's target serialization; synchronous gate entry from the target callback would invert that order and can deadlock across threads.
Use a non-waiting handoff and perform the gate-dependent operation after the target callback has returned; a marker set inside the callback does not by itself establish that return.
`preparePublish()` and `prepareClear()` only commit graph facts under the registry state mutex, not the delivery gate: a non-render notification may prepare a ticket, retain it, and defer its delivery.
That distinction permits no new render-domain work or synchronous backend controls from render callbacks.
This deliberately narrows raw callback composition rather than changing the locking mechanism; the cross-thread gate-order precondition is not runtime-enforced.
Native SDK calls and close settlement may block; the boundary is lifetime safety, not a wait-free timing guarantee.
Close retires native listeners and the AudioUnit before delivering the prepared clear.
Core Audio additionally diagnoses same-backend target-origin open/close misuse at runtime: every RenderTarget callback boundary is marked with the shared `utility::CallbackStackScope` identity, and `open()` and `close()` assert that identity is absent before mutating target or native state.
That assertion covers misuse reached through a graph callback nested beneath a target callback.
It reports the defect rather than recovering from it: the deferral requirement above still governs, and a rejected call neither succeeds nor substitutes a deferred replacement.
Ordinary raw graph-observer close and the run-only start/stop supported above fall outside the check and remain allowed.
A separate per-backend render-domain marker rejects `start()`, `pause()`, `resume()`, `flush()`, and `stop()` before control locking when called from PCM, underrun, position, or ordered drain-complete callbacks.
Drain completion runs on the control worker rather than the realtime thread, but remains part of the serialized render-producer domain.
The marker is distinct from the general target-callback identity, so the supported route/format-origin start/stop paths remain allowed.
These diagnostics cover only the named Core Audio mutation boundaries, not every SDK or native-fault path, and other backends are not promised equivalent runtime diagnostics; they remain bound by the Backend callback preconditions.
Property writes retain synchronous native error reporting and cached intent, but coalesce graph-publication requests on the existing non-realtime control worker when a graph registry is present.
That handoff avoids outward graph delivery on a target callback's property-write stack and publishes current state rather than replaying an old property snapshot.

The callback gate serializes delivery without holding the registry state mutex across callbacks.
Each publisher retains responsibility for its own thread's delivery: a different publishing thread may update the route revision while waiting for the gate, but never transfers its callbacks to the active publisher's thread.
Only same-thread reentrant work joins the active delivery drain.
This does not authorize callbacks to wait for another thread's publication or cancellation while retaining the gate.

Registry retirement is a separate synchronous path, not ordinary queued delivery.
Shutdown discards pending ordinary deliveries, closes admission, and sends final empty graphs to subscriptions that have not been cancelled.
An external shutdown caller waits behind in-flight callbacks; after retirement returns, no new callback starts and retained publishers are inert.
Shutdown initiated on a graph callback stack can synchronously nest final-empty callbacks so that observer state can be released after that shutdown call returns; ordinary non-nesting does not weaken this lifetime boundary or the provider's separate quiescence contract.

### ALSA mixer controls

ALSA mixer initialization, repeated initialization, candidate rejection, and close never write shared volume or switch controls.
Selection checks active playback-volume capability, valid ranges, and readable playback channels without proving writability by changing their values.
Hardware assistance describes the selected control mechanism, not a successful speculative write test.
A cached application volume is not replayed onto a newly selected hardware mixer during open; the backend reads the device's existing volume instead.
When no usable mixer exists, the cached software gain is applied in software.
A successful hardware write does not populate that software-gain cache: if a later reopen cannot select hardware, the already-applied attenuation is not applied a second time to PCM.

Hardware volume observations and explicit writes first process mixer events and resolve the selected element again by name and index.
An element pointer never survives this refresh boundary: removal, replacement, or changed capabilities require a new readable snapshot.
Read-only hardware switch observations contribute to effective graph mute state, but Aobus never changes those switches.
The ALSA Muted property and playback-session persistence expose only application mute intent.
That pure application-mute getter performs no native refresh; the Volume property owns the native mixer observation and publishes its resulting graph after releasing the mixer mutex.
Application mute always silences the application's PCM samples, independently of the volume-control mechanism; unmute does not clear an external system mute.
A cached application mute therefore remains effective on the first playback open, including with hardware volume control.
When unmuted and using hardware volume, the software gain is unity and PCM bytes are unchanged.

#### What happens when observation or a write fails?

If refresh fails or the selected element becomes unusable, the backend switches to software control with unity gain.
When that observation failure occurs before an explicit volume write, the request reports an I/O error that states no volume write was attempted.
Refresh and read failures do not exclude the element: the same stable name-and-index identity may be reconsidered after a later PCM reopen.

Immediately before calling a hardware setter, the backend records that element identity while it still holds the mixer mutex.
This allocation must happen before the hardware write because allocation failure cannot be allowed after a setter may already have changed one or more channels.
A successful setter removes the provisional record.
A failed setter leaves the identity excluded for this backend's lifetime, including PCM close/reopen, but does not exclude another readable candidate; a newly constructed backend starts with no exclusions.
The error reports that hardware may have changed partially, switches to unity software fallback, and does not apply the requested attenuation again in software.
There is no compensating write, rollback guarantee, or automatic restoration to maximum volume.
The next explicit volume request controls software gain, which remains cached across a reopen that finds no other hardware candidate.

Each published mixer graph takes volume, effective mute, and control mechanism from one mutex-consistent snapshot.
A Volume-property observation that changes or loses the selected element updates the registry's corresponding fallback graph before returning its scalar value and capability can be queried.
Observer delivery follows the [backend graph delivery contract](#backend-graph-delivery), so a change discovered inside a graph callback is delivered after that callback returns rather than recursively.
Close makes the mixer unavailable before clearing the graph, with no mixer or observation mutex held across graph callbacks.
An observation during the clear callback or after close returns the scalar fallback without recreating the cleared graph; unknown properties neither observe nor publish mixer state.
Explicit control requests can still publish their state before PCM open; only unavailable scalar observations suppress publication.
Control mutations derive and atomically publish the complete software render gain while holding the mixer mutex; the render loop loads that single value once per PCM batch rather than combining independent mute, mode, and gain observations.
Application mute publishes its render gain before refreshing hardware observations, so mixer I/O cannot delay that publication.
An in-flight render batch may retain an earlier complete gain, and already queued PCM is not rewritten; no one-period audible-response guarantee is implied.

#### Which scale maps normalized volume?

Raw and dB mappings select their exact endpoints for zero and unity rather than rounding a scaled offset at those boundaries.
An ordinary readable dB range uses dB mapping.
If its minimum is ALSA's `SND_CTL_TLV_DB_GAIN_MUTE` sentinel, however, that minimum is not a finite gain suitable for interpolation; reads and writes therefore use the element's raw scale instead.
Integer-offset arithmetic preserves narrow ranges near native integer limits.
NaN requests return `InvalidInput` before mixer locking, refresh, graph publication, or gain and intent mutation, while infinities clamp to their respective endpoints.

### Shutdown

The first `Engine::shutdown()` caller changes lifecycle under the control lock, retires the render session, stops and joins the event worker, and closes backend/timeline state.
Commands admitted after lifecycle transition do not enter backend/timeline logic and result-bearing commands return `InvalidState`.
Concurrent shutdown callers wait for the single teardown; repeated completed shutdown is a no-op.
Engine event-queue destruction requires a non-joinable worker, a published
stopped state, an empty non-realtime event deque, and an empty realtime ring.
Those are always-active invariants in every build configuration.

Player public methods and destruction run on its executor, which outlives Player.
Destruction closes the shared gate and cancels start/lookahead task handles before subscriptions, providers, and Engine stop, so already queued tasks return without touching Player state.
Each provider's first shutdown caller starts retirement.
A concurrent caller outside that provider's callback or monitor thread waits for the same provider-quiescence completion; repeated completed shutdown is a no-op.
A callback- or monitor-thread initiator returns without self-wait after closing admission and starting retirement, while the callback/worker's independent shared control state finishes completion as it unwinds; a later external caller still waits for that completion.
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
A backend activation failure classified `OutputFailure` is a non-recoverable route-activation outcome for that start; an explicitly typed `TrackRouteIncompatible` refusal keeps its recoverable track-open classification.
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
- Audio detail timeline and track-session code under [`lib/audio/detail/`](../../../lib/audio/detail) owns nodes and decode lifetime; source-private [`StreamingSource`](../../../lib/audio/StreamingSource.h), [`PcmFrameRing`](../../../lib/audio/PcmFrameRing.h), and [`StreamingBufferPolicy`](../../../lib/audio/detail/StreamingBufferPolicy.h) own PCM production, bounded storage, and producer admission.
- [`Player.h`](../../../include/ao/audio/Player.h) and [`Player.cpp`](../../../lib/audio/Player.cpp) own provider composition, executor marshalling, graph epochs, and teardown gate.
- [`BackendProvider.h`](../../../include/ao/audio/BackendProvider.h) owns the common provider facade, callback, subscription, backend-outlives-provider, and shared shutdown-completion contract.
- [`Backend.h`](../../../include/ao/audio/Backend.h), the private [`DecoderOutput.h`](../../../lib/audio/detail/DecoderOutput.h), and concrete providers/backends under [`lib/audio/backend/`](../../../lib/audio/backend) own advisory prediction, narrow provider control/registry state, lossless candidate derivation, native selection, and lifetime. [`BackendDeviceRegistry`](../../../lib/audio/backend/detail/BackendDeviceRegistry.h), [`BackendGraphRegistry`](../../../lib/audio/backend/detail/BackendGraphRegistry.h), and the ALSA graph publisher provide provider-independent subscription/publication retirement.
- [`PipeWireFormatParsing.cpp`](../../../lib/audio/backend/detail/PipeWireFormatParsing.cpp) and [`PipeWireBackend.cpp`](../../../lib/audio/backend/PipeWireBackend.cpp) own the PipeWire client offer, readback admission, immutable render delivery, and open snapshot. [`PipeWireMonitor.cpp`](../../../lib/audio/backend/PipeWireMonitor.cpp) owns the independent current Format cache used for route-graph channel evidence.
- [`AlsaMixerSession.cpp`](../../../lib/audio/backend/detail/AlsaMixerSession.cpp) owns read-only mixer selection, event refresh and stable element identity, software mute, and safe volume-failure degradation.
- [`PlaybackTransport.cpp`](../../../app/runtime/playback/PlaybackTransport.cpp) owns executor-affine transport adaptation and prepared metadata; [`PlaybackService.cpp`](../../../app/runtime/playback/PlaybackService.cpp) publishes the coherent application snapshot.

## Test map

- [`EngineConcurrencyTest.cpp`](../../../test/unit/audio/EngineConcurrencyTest.cpp) protects concurrent commands, status/seek serialization, render/reset exclusion, and teardown; Linux-only [`AlsaEnginePropertyTest.cpp`](../../../test/unit/audio/backend/AlsaEnginePropertyTest.cpp) protects reentrant state-only queries during initial graph subscription, ALSA property publication, read-triggered fallback, and volume/mute intent across stop after successful or rejected hardware writes.
- [`BackendGraphRegistryTest.cpp`](../../../test/unit/audio/backend/detail/BackendGraphRegistryTest.cpp) protects graph deduplication, non-nested ordinary and initial delivery, prepared commit/delivery separation, move-only ticket ownership, revision supersession, cancellation, and synchronous final-empty retirement.
- Linux-only [`PipeWireBackendAdmissionTest.cpp`](../../../test/unit/audio/backend/PipeWireBackendAdmissionTest.cpp) drives the registered native callback table through a scoped external callback-source fake. It protects open-window evidence projection, literal render-byte delivery, terminal Format/ERROR revocation, and restart refusal before Engine settlement. Synchronous scripted dispatch does not prove native realtime affinity, daemon negotiation behavior, or asynchronous callback fences.
- [`CoreAudioBackendTest.cpp`](../../../test/unit/audio/backend/CoreAudioBackendTest.cpp) protects native publication and initial-observer close, property-publication worker handoff with observer-driven close, open-origin notification settlement against external close, final render quiescence after notification-origin restart, current open notifications across run-only changes, and synchronous registry retirement during native publication.
  It also covers deferring a target-origin graph subscription until the open call returns while an observer on another thread holds the shared delivery gate and requests stop; request markers do not prove private mutex admission.
  The different-backend identity case uses an early-refused synthetic open and an inert close, not successful cross-backend shared-gate entry.
  Apple-only [`CoreAudioBackendRunStateTest.cpp`](../../../test/unit/audio/backend/CoreAudioBackendRunStateTest.cpp) drives the real Backend through constructor-injected native control/readback results, covering failed-stop admission/reset gates, current-generation error suppression, and drain/fault settlement. It uses a real output unit but does not demonstrate an actual SDK stop failure or physical routing.
  Apple-only [`CoreAudioBackendFatalProbeTest.cpp`](../../../test/unit/audio/backend/CoreAudioBackendFatalProbeTest.cpp) protects the Core Audio target callback-origin misuse diagnostics and the device-free render-admission/native-run preconditions through child-process fatal probes; it exercises those owned diagnostic scenarios, not every native fault producer path.
  Its nested-graph scenarios deliberately violate the gate-deferral precondition to reach the owned lifecycle diagnostic; they do not establish support for synchronous shared-registry entry from target callbacks.
- [`CoreAudioChannelLayoutTest.cpp`](../../../test/unit/audio/backend/detail/CoreAudioChannelLayoutTest.cpp) protects observation bounds, role admission, byte permutations, required readback failures and execution/observation refresh; [`CoreAudioGraphTest.cpp`](../../../test/unit/audio/backend/detail/CoreAudioGraphTest.cpp) protects sink-only conversion findings and [`CoreAudioRenderBufferTest.cpp`](../../../test/unit/audio/backend/detail/CoreAudioRenderBufferTest.cpp) protects nonmutating shape refusal.
- [`CoreAudioControlMailboxTest.cpp`](../../../test/unit/audio/backend/detail/CoreAudioControlMailboxTest.cpp) protects pending-generation arbitration; [`CoreAudioRunControlTest.cpp`](../../../test/unit/audio/backend/detail/CoreAudioRunControlTest.cpp) protects consumed-action ordering. These controlled cases do not establish native SDK or callback-lifetime behavior.
- [`CoreAudioChannelObservationTest.cpp`](../../../test/unit/audio/backend/CoreAudioChannelObservationTest.cpp) is a hidden exact-UID disposable-AUHAL map readback probe, not rendered/captured PCM or hardware routing proof.
- [`EngineRtSignalRingTest.cpp`](../../../test/unit/audio/EngineRtSignalRingTest.cpp) protects the exact two-entry capacity, serialized producer handoff, sequential-splice occupancy, pending-drain arm behavior, and legal full-ring delivery.
- [`EngineFatalProbeTest.cpp`](../../../test/unit/audio/EngineFatalProbeTest.cpp) and the self-reentering `ao_audio_fatal_probe` under [`test/fatal/`](../../../test/fatal) protect realtime overflow, timeline-owner, and event-queue destruction fatal invariants in a child process.
- [`EngineTest.cpp`](../../../test/unit/audio/EngineTest.cpp) protects optimistic explicit-start PCM selection, prepared-source reuse, and exact backend-mode fallback.
- [`EngineSeekErrorTest.cpp`](../../../test/unit/audio/EngineSeekErrorTest.cpp) protects Error rejection through both seek entry points before and after failure delivery, paused-origin failure, unchanged backend/source state on rejection, and recovery through a new playback.
  [`EngineControlErrorTest.cpp`](../../../test/unit/audio/EngineControlErrorTest.cpp) protects terminal stop-callback delivery across a later successful seek sequence and old-target rejection during explicit replacement; its backend fake does not reproduce an SDK stop failure.
- [`EngineGaplessTest.cpp`](../../../test/unit/audio/EngineGaplessTest.cpp), [`EngineDrainTest.cpp`](../../../test/unit/audio/EngineDrainTest.cpp), and [`AudioBackendRenderProgressTest.cpp`](../../../test/unit/audio/backend/detail/AudioBackendRenderProgressTest.cpp) protect splice, cross-precision mode reuse, drain, mixed-buffer progress, and fallback.
- [`EngineStereoReuseTest.cpp`](../../../test/unit/audio/EngineStereoReuseTest.cpp) protects backend-authorized WAV/ALAC and FLAC/ALAC stereo mask-alias transport reuse, real FLAC/ALAC strict draining, identity preservation, retained endpoint precision, and adoption-time rechecking; native value tests separately protect the transport proof, not physical routing.
- [`EngineFlacChannelLayoutTest.cpp`](../../../test/unit/audio/EngineFlacChannelLayoutTest.cpp) owns real WAV/FLAC transition coverage: matching default identities permit strict reuse for counts 1/2/3/4/7/8, while absent-mask five/six-channel FLAC versus inferred WAV drains/reopens. It also covers captured PCM, successor interpretation publication, different-role drain/reopen, malformed-outer-metadata refusal before backend open, and rejected lookahead replacement.
- [`EngineFlacTransitionTest.cpp`](../../../test/unit/audio/EngineFlacTransitionTest.cpp) covers six-channel real-FLAC producer transitions with equal-known and equal-unknown reuse controls, known↔unknown and different-role drain/reopen, captured PCM, and successor evidence publication.
  These controlled Engine backends are not native hardware, audibility, or performance evidence.
- [`EngineCallbackTest.cpp`](../../../test/unit/audio/EngineCallbackTest.cpp), [`EngineErrorTest.cpp`](../../../test/unit/audio/EngineErrorTest.cpp), and [`EngineBackendSwapTest.cpp`](../../../test/unit/audio/EngineBackendSwapTest.cpp) protect generations, stale events, typed failures, and synchronous invariant exceptions.
- [`PlayerTest.cpp`](../../../test/unit/audio/PlayerTest.cpp) protects executor marshalling, responsive worker-side preroll, cancellation cleanup, asynchronous diagnostic boundaries, graph epochs, and gate behavior.
- [`AlsaProviderTest.cpp`](../../../test/unit/audio/backend/AlsaProviderTest.cpp), [`PipeWireMonitorTest.cpp`](../../../test/unit/audio/backend/PipeWireMonitorTest.cpp), [`WasapiProviderTest.cpp`](../../../test/unit/audio/backend/WasapiProviderTest.cpp), and [`CoreAudioProviderTest.cpp`](../../../test/unit/audio/backend/CoreAudioProviderTest.cpp) protect callback-destroys-provider, late subscription/backend teardown, subscribe/shutdown races, nested-callback suppression, monitor exit, and concurrent external callers sharing provider quiescence.
- [`AlsaExclusiveBackendTest.cpp`](../../../test/unit/audio/backend/AlsaExclusiveBackendTest.cpp), [`AlsaModeSelectorTest.cpp`](../../../test/unit/audio/backend/detail/AlsaModeSelectorTest.cpp), [`AlsaPcmFormatTest.cpp`](../../../test/unit/audio/backend/detail/AlsaPcmFormatTest.cpp), and [`AlsaPcmErrorTest.cpp`](../../../test/unit/audio/backend/detail/AlsaPcmErrorTest.cpp) protect direct-hardware enforcement, scalar-observation graph publication, software fallback retention across reopen after a mixer write failure, closed-graph retirement, strict lossless selection, significant-bit evidence, exact native format mapping, and open-error classification.
- [`StreamingSourceTest.cpp`](../../../test/unit/audio/StreamingSourceTest.cpp), [`PcmFrameRingTest.cpp`](../../../test/unit/audio/PcmFrameRingTest.cpp), and [`StreamingBufferPolicyTest.cpp`](../../../test/unit/audio/detail/StreamingBufferPolicyTest.cpp) protect decode-worker lifetime, bounded producer admission, oversized and partial-frame blocks, whole-frame ring transfer, constant-time reset reuse, and source retirement; [`StreamingRenderAlignmentTest.cpp`](../../../test/unit/audio/StreamingRenderAlignmentTest.cpp) protects lossless whole-frame reads and Engine render alignment across a parked partial write, and [`TrackSessionTest.cpp`](../../../test/unit/audio/detail/TrackSessionTest.cpp) protects frameless-format rejection.
- [`AlsaMixerSessionTest.cpp`](../../../test/unit/audio/backend/detail/AlsaMixerSessionTest.cpp) protects zero-write lifecycle/mute operations, refreshed hardware state, element replacement/removal, volume failures, numeric endpoints, mute-sentinel raw-scale mapping, successful-write eligibility after reopen, failed-element exclusion across reopen, independent candidate and session identities, and refresh/read failures without exclusion.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp) protects deferred cold restore without opening output; [`PlaybackSessionVolumeTest.cpp`](../../../test/unit/runtime/PlaybackSessionVolumeTest.cpp) checks exact unity and near-unity YAML round trips; Linux-only [`PlaybackSessionAlsaMuteTest.cpp`](../../../test/unit/runtime/PlaybackSessionAlsaMuteTest.cpp) protects application-mute save/restore against external ALSA switch state and exact hardware-volume intent through public stop snapshots, checkpoint, shutdown, and restore.
- Runtime playback tests under [`test/unit/runtime/`](../../../test/unit/runtime) protect executor-affine publication and application metadata.

## Related documents

- [Playback architecture](README.md)
- [PCM representation and lossless output](pcm-format.md)
- [Runtime execution architecture](../execution/README.md)
- [Failure and reporting architecture](../failure/README.md)
- [Playback succession cursor](cursor.md)
- [Decoder session](decoder-session.md)
- [Audio quality analysis](quality-analysis.md)
