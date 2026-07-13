---
id: rfc.0028.bounded-audio-observation-delivery
type: rfc
status: draft
domain: playback
summary: Proposes bounded, generation-aware, coalescing delivery of audio observations from engine and provider threads to runtime callback state.
depends-on: none
---
# RFC 0028: Bounded audio observation delivery

## Problem

Aobus has strong thread ownership and teardown rules across Engine, Player, PlaybackService, UIModel, and frontends.
The Engine render boundary uses a fixed-capacity realtime signal ring, Engine serializes non-realtime events on its event worker, and Player gates callbacks before dispatching them to the runtime callback executor.

The end-to-end observation path is nevertheless not bounded as one system:

```text
backend / render / decoder / provider
  -> Engine event queue
  -> Player CallbackGate dispatch
  -> callback executor queue
  -> PlaybackService events/snapshot
  -> observers
```

Engine's non-realtime `PlaybackEvent` queue is an unbounded `std::deque`.
Each Engine state/route/track/failure callback and provider device/graph callback can create a separate Player executor task.
`CallbackGate` rejects work after shutdown, but while open it has no capacity, coalescing, one-drain flag, or overflow policy.
The frontend executor can therefore accumulate stale intermediate observations when a producer runs faster than the GTK/TUI/headless consumer.

Not all observations have the same semantics.
A track advance, terminal failure, or accepted route-generation transition cannot be treated like a replaceable status refresh.
Repeated state-changed, route snapshot, provider-device, or quality recomputation signals can often collapse to the latest generation.
Today that classification is implicit in callback handlers rather than encoded at the delivery boundary.

Generation checks occur inside several queued Player tasks, after those tasks have already consumed executor queue capacity.
Old playback/route/provider generations can therefore create arbitrary queued work even when the eventual handler discards them.

The fixed realtime ring asserts and returns false on overflow, but the complete product contract does not say how capacity relates to event-worker latency, how overflow becomes a safe playback failure, or how a saturated non-realtime/callback path avoids losing a required transition.

This is a production scaling and latency risk rather than evidence of a current audible failure.
Device churn, repeated route graph changes, backend error storms, or a blocked callback executor can turn an otherwise correct lifetime design into memory growth and delayed stale UI/runtime state.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0005](0005-coherent-playback-boundary.md), [RFC 0011](0011-executor-affine-reporting-feed.md), [RFC 0012](0012-structured-async-fault-diagnostics.md).

RFC 0005 must align coherent PlaybackService publication with observation batches and overflow recovery.
RFC 0011 should consume bounded authoritative playback updates without re-expanding coalesced telemetry into an unbounded reporting feed.
RFC 0012 should receive unexpected bridge faults and overflow diagnostics without duplicating the user-facing playback failure.

## Goals

- Bound memory and queued callback work for every audio/provider-to-runtime observation path.
- Preserve lossless ordering for semantic transitions that cannot be coalesced.
- Coalesce replaceable telemetry/snapshots by kind and generation before callback-executor queue growth.
- Schedule at most one callback-executor drain per Player observation bridge.
- Reject stale generations before expensive copying or executor admission where possible.
- Keep realtime producers non-blocking, allocation-free, and bounded.
- Define explicit overflow, degradation, and playback-quiescence behavior for each event class.
- Close observation admission and settle/discard queued work deterministically during teardown.
- Expose metrics and deterministic stress tests for capacity, coalescing, staleness, and latency.

## Non-goals

- Change audio succession, gapless transition, route-quality, or frontend presentation semantics.
- Deliver PCM, meters, or waveform data through the application callback executor.
- Treat every observation as lossless or every observation as latest-wins.
- Block a realtime render callback waiting for Engine, Player, runtime, or UI work.
- Replace the callback executor or reporting feed.
- Guarantee zero allocation on Engine's non-realtime event worker; the hard realtime boundary remains narrower.

## Proposed design

### Observation taxonomy

Define a closed internal taxonomy at the Engine/Player boundary.
Each observation kind declares one delivery class:

| Class | Examples | Delivery rule |
|---|---|---|
| Lossless transition | accepted track advance/end, terminal playback failure, explicit route loss requiring transport change | Ordered exactly once for the accepted generation. |
| Coalescible snapshot | state changed, current route snapshot, provider device snapshot, quality/readiness snapshot | Retain only the newest accepted value for its key/generation before a drain. |
| Diagnostic telemetry | repeated equivalent backend/source detail that does not change semantic state | Count/sample under a bounded policy and report through diagnostics, not as unlimited application events. |
| Realtime signal | drained/spliced handoff from render | Fixed-capacity non-blocking ingress with preallocated evidence and explicit overflow fail-safe. |

The table becomes code metadata or typed variants, not comments duplicated among handlers.
Adding an observation kind requires selecting its class, key, generation evidence, overflow rule, and test.

### Engine event ingress

Retain the existing fixed realtime SPSC ring and make its capacity/overflow contract explicit.
Realtime enqueue performs no allocation, lock, logging, formatting, or callback invocation.

Replace the unbounded non-realtime deque with a bounded event queue that supports:

- reserved capacity for lossless terminal/transition events;
- keyed latest-value slots for coalescible Engine snapshots; and
- one counted diagnostic for repeated/overflowed telemetry.

Non-realtime producers may briefly take the existing queue mutex but cannot grow memory beyond configured capacity.
They do not block indefinitely waiting for the event worker.

When a lossless event cannot be admitted despite its reserve, Engine enters a defined fail-safe state: reject further observation admission for that playback generation, quiesce/stop the affected playback path through a control-safe mechanism, and publish one reserved overflow failure when the worker can run.
It must not continue playback after silently losing a terminal transition.

Realtime ring overflow uses an equivalent preallocated fail-safe indication rather than relying only on a debug assertion.
The render callback still returns immediately.

### Player observation bridge

Replace one-executor-task-per-callback with one `PlayerObservationBridge` owned by Player and backed by shared teardown state.

Lower callbacks submit typed observations into a bounded synchronized ingress.
Submission performs early generation rejection using atomically readable accepted playback/route/provider generations.
It then:

- appends a lossless transition to reserved ordered capacity;
- replaces the previous value in a coalescible slot; or
- increments bounded diagnostic evidence.

The first accepted observation changes a `drainScheduled` flag and dispatches one drain task to the callback executor.
Further observations update the bounded ingress without dispatching more tasks while that drain is pending.

On the callback executor, the drain atomically extracts one batch, clears or hands off the scheduling flag without a lost-wakeup race, revalidates generation, applies observations in defined order, and invokes outward callbacks.
If producers raced with the drain, exactly one later drain remains scheduled.

### Batch ordering and publication

Within one extracted batch:

1. discard observations whose generation is no longer accepted;
2. process lossless track/failure/route transitions in Engine order;
3. recompute/install the latest coherent Player state and quality evidence; and
4. publish at most one coalesced callback for each replaceable outward snapshot kind.

PlaybackService receives a batch/receipt or the same callbacks with an explicit publication guard, but it cannot observe a quality/status snapshot from before a lossless advance that the same batch has already accepted.

Synchronous Player control commands remain authoritative for their immediate return state.
The observation bridge carries asynchronous lower-layer changes and cannot overwrite a newer synchronous generation.

### Capacity and overload policy

Capacity values are internal product contracts with injectable test limits.
They are selected from measured worst-case route/device/transition bursts and include separate reserves; one large provider snapshot cannot consume the lossless transition reserve.

Expose bounded counters for:

- coalesced replacements by kind;
- stale drops by generation;
- diagnostic samples suppressed;
- high-water marks;
- non-realtime queue overflow; and
- realtime overflow/fail-safe activation.

Counters are observational and must not create another high-frequency callback stream.
Unexpected overflow is logged/diagnosed once per generation and mapped to one playback failure/recovery owner.

### Teardown

Shutdown proceeds producer-first:

1. close provider/Engine observation admission for new generations;
2. unsubscribe and stop provider producers;
3. stop/join Engine event and backend/decode producers;
4. mark the Player bridge closing;
5. on the callback executor, drain required already-accepted lossless transitions or explicitly discard them under shutdown policy;
6. clear coalescible slots and outward callbacks; and
7. release the shared gate after no producer can enqueue.

Queued executor drain tasks retain only shared bridge control state.
After close they cannot dereference Player or PlaybackService.
The bridge reports whether required work remained, making accidental teardown loss visible in tests.

### Reporting-feed interaction

Playback state remains authoritative in PlaybackService snapshots.
UIModel/frontends observe the coalesced application state, not raw Engine event counts.

The reporting feed receives semantic playback failures and bounded diagnostics.
It does not mirror every discarded state-changed callback.
This preserves RFC 0011's bounded authoritative retention rather than moving pressure downstream.

## Alternatives

### Increase executor and queue capacity

A larger unbounded queue only delays memory/latency failure and still retains stale replaceable work.
Classification and coalescing remove unnecessary work before capacity is consumed.

### Coalesce only in PlaybackService

By then every lower callback has already allocated and occupied the Player/executor queues.
Backpressure must begin at Engine/Player ingress while PlaybackService still owns semantic publication.

### Drop all observations when full

Dropping a status refresh can be safe; dropping a track advance or terminal failure is not.
Event classes need distinct reserves and fail-safe behavior.

### Block producers until the callback executor catches up

Blocking render is forbidden, and blocking Engine/provider threads can deadlock teardown or audio control.
Bounded admission with coalescing and explicit overload is safer.

### Poll Engine state from the frontend

Polling adds latency, duplicated timers, and lost transition semantics, and it does not solve provider/terminal event delivery.

## Compatibility and migration

Public playback commands, snapshots, and user-visible success behavior remain unchanged.
Intermediate duplicate state/quality/device callbacks may be fewer, but every observer is already required to treat the latest runtime snapshot as authority.

Lossless transition order remains stable and becomes explicitly protected.
Under overload, behavior changes from unbounded delay or debug assertion to a bounded diagnosed fail-safe playback outcome.

Implementation can migrate in two stages: first the Player one-drain bridge, then bounded/coalescing Engine non-realtime ingress and explicit realtime overflow.
Each stage must retain generation and teardown tests; no stage may add blocking work to render.

## Validation

- A producer storm leaves Engine, Player bridge, and callback-executor queued work within configured bounds.
- Coalescible state/route/device/quality updates deliver the newest accepted value and at most one outward callback per drain batch.
- Lossless track advances, ends, and failures preserve accepted order and exactly-once delivery.
- Old playback, route, and provider generations are rejected before executor task proliferation.
- Lost-wakeup tests cover enqueue before, during, and after drain flag transitions.
- Capacity-one/tiny-limit tests force every overflow branch, reserved slot, and fail-safe outcome deterministically.
- Realtime instrumentation proves enqueue remains non-blocking and allocation-free.
- A blocked callback executor does not cause unbounded memory growth; high-water/coalescing counters reflect the pressure.
- PlaybackService never installs a stale pre-transition snapshot after accepting a newer transition.
- Teardown tests cover queued drain, producer race, provider unsubscribe, Engine stop, and no callback after target destruction.
- TSAN stress covers Engine event ingress, Player bridge, generation changes, and shutdown.
- Existing playback, gapless, route, quality, provider, GTK, and TUI behavior tests pass, followed by a full `./ao check`.

## Open questions

- Which exact existing Engine/Player callbacks are lossless transitions versus coalescible snapshots?
- Should the Player bridge use a fixed ring plus keyed slots, a bounded deque with replacement indices, or a typed mailbox?
- What capacities and reserves follow from measured provider/device/route worst cases?
- Can lossless overflow always quiesce safely from the detecting thread, or does it require a preallocated control signal to the event worker?
- Should PlaybackService consume one explicit batch API or retain callbacks with a shared publication epoch?

## Promotion plan

If accepted and implemented:

- update the [playback architecture](../architecture/playback.md) with observation classes, bounded ingress, one-drain delivery, and overload teardown;
- update the [runtime execution architecture](../architecture/runtime-execution.md) with the bounded audio-to-callback bridge;
- update the audio execution/concurrency specification with exact event classes, capacities, realtime constraints, and overflow behavior;
- update playback failure and audio-quality specifications with coalesced publication and fail-safe outcomes;
- update failure/reporting documentation with bounded overflow diagnostics and single-owner presentation; and
- add contributor guidance and deterministic concurrency helpers for tiny-capacity observation tests.
