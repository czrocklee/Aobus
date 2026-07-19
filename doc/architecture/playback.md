---
id: architecture.playback
type: architecture
status: current
domain: playback
summary: Defines playback ownership, execution domains, and cross-boundary protocols from live succession through audio output and session persistence.
---
# Playback architecture

## Scope

This document owns the structural model for interactive playback from frontend intent through live-library succession, application transport, audio execution, platform output, and session persistence.
It identifies state authorities, ownership and dependency direction, execution domains, cross-boundary protocols, and teardown order.

It does not define next/previous resolution, shuffle or repeat transitions, prepared-next race outcomes, decoder format guarantees, quality classifications, failure-recovery matrices, or the persisted session schema.
Those behavioral and exact-surface facts remain in the linked specifications and legacy playback documents until their migration has a complete new owner.

## System context

Playback exists only in the interactive `AppRuntime` composition above `CoreRuntime`.
GTK and TUI provide platform audio providers and consume playback through runtime and UIModel boundaries; the CLI `CoreRuntime` composition has no interactive playback stack.

The ownership graph is:

```text
frontend widgets / platform adapters
             |
             v
UIModel playback command and view models
             |
             v
AppRuntime
  |-- ViewService + TrackSourceCache
  |         |
  |         v
  |   PlaybackSequenceService
  |     `-- PlaybackCursorSession
  |           |-- source lease + detached live projection
  |           |-- cursor + anchor + shuffle history
  |           `-- prepared-next registry
  |
  |-- PlaybackSessionPersistence
  |         | observes and coordinates
  |         v
  `-- PlaybackService
        `-- ao::audio::Player
              |-- BackendProvider instances + route/quality state
              `-- ao::audio::Engine
                    |-- current/lookahead audio timeline
                    |-- Engine event worker
                    |-- StreamingSource decode workers
                    `-- active Backend render/device boundary
```

The graph crosses the top-level layers from the [system architecture](system-overview.md):

| Playback area | System layer | Public code boundary | Implementation |
|---|---|---|---|
| Sequence, transport, and session lifecycle | Application runtime | `app/include/ao/rt/` | `app/runtime/` and `app/runtime/playback/` |
| Commands and display adaptation | UIModel | `app/include/ao/uimodel/playback/` | `app/uimodel/playback/` |
| Player, engine, decode, routing, and backend interfaces | Core libraries | `include/ao/audio/` | `lib/audio/` |
| Provider construction and native presentation | Frontends/platform adapters | Frontend-local | `app/linux-gtk/` and `app/tui/` |

Playback is not one state machine stretched across these layers.
It has an application semantic domain and a core audio execution domain connected by explicit start, prepared-transition, and observation protocols; `PlaybackSessionPersistence` coordinates durable intent across the two application authorities.

## Responsibilities

### Succession authority

`PlaybackSequenceService` owns where playback comes from and where it may move next.
It owns the captured launch context, source identity, current cursor session, next/previous availability, shuffle and repeat modes, source invalidation, prepared-next correlation, and recovery decisions that require live-source context.

`PlaybackCursorSession` owns the lease-pinned source dependency graph and a detached live projection for one playback conversation.
It groups the cursor, projection anchor, shuffle history, and prepared-next registry without materializing a second frontend-provided queue.
`ViewService` supplies a coherent launch description, but the session remains valid independently of the originating view.

### Application transport authority

`PlaybackService` owns the application-facing current-subject and transport boundary.
It resolves a library `TrackId` into a runtime playback request, owns the now-playing snapshot, translates pause/resume/stop/seek and output commands, exposes volume and quality state, and publishes executor-affine application observations.

It does not decide list membership, ordering, shuffle, repeat, or source recovery.
Sequence-only operations use a private collaboration surface so ordinary consumers cannot update transport without the matching succession policy.

### Session-persistence coordinator

`PlaybackSessionPersistence` owns the composite durable listening intent rather than either playback authority owning a partial payload.
It observes discrete persistence intent from sequence and transport, coordinates save scheduling and retry, validates stored input, prepares a restore candidate, and installs sequence and deferred transport coherently.

It borrows the runtime library, both playback authorities, `ConfigStore`, and `async::Runtime` from `AppRuntime` composition.
It does not become a third live playback-state authority: current sequence and transport snapshots remain owned by their services.

### Player bridge and route authority

`ao::audio::Player` is the application-facing owner of the core audio collaborator graph.
It exclusively owns registered `BackendProvider` instances and one `Engine`, selects the active output, combines provider and engine route graphs, analyzes delivery quality, and marshals lower observations onto the runtime callback executor.

Player public methods and mutable application-facing state are callback-executor-affine even though the owned Engine is thread-tolerant.
Its callback gate separates queued lower-layer observations from Player lifetime and drops work that has not been accepted before shutdown.

### Audio execution authority

`ao::audio::Engine` owns audio transport execution below application semantics.
It owns the active backend attachment, current and lookahead audio items, playback and route generations, render-session correlation, synchronized status, and the event worker that serializes non-realtime source/backend events and realtime transition signals.

Engine item identity is opaque with respect to application identity and carries no TrackId or ListId semantics.
Engine does not know library track or list identities, views, shuffle, repeat, session persistence, notifications, or frontend state.

### Decode and output boundaries

Engine opens an audio item into a decoder-backed PCM source through its track-session construction path.
`StreamingSource` owns the decoder, its decode thread, PCM ring buffer, seek cancellation, and source-error handoff.
Decoder implementations may reuse container primitives from `ao_media`, but the [encoded media architecture](encoded-media.md) keeps encoded-file reading and decoder consumption as independent capabilities; neither media file reading nor its visitor owns decoder-session state or PCM behavior.

`BackendProvider` owns platform output discovery and creates `Backend` instances for selected devices and profiles.
A provider descriptor carries only stable backend/profile ids and supported-profile structure.
Device display names and descriptions discovered from the operating system remain external facts, while built-in backend/profile labels, descriptions, and icon meaning do not enter Core audio.
A Backend owns its native output handles, render/control resources, and platform threads; its realtime callback consumes PCM through the narrow source/render boundary and reports non-realtime state back to Engine.

### UIModel and frontend adapters

UIModel owns reusable command availability, transport presentation, seek interpolation, output-device presentation, and quality formatting.
Its presentation catalog maps known backend/profile ids to shared labels and semantic `AudioIconKind` values, with stable-id fallback for unknown extensions.
GTK maps the semantic kind to a symbolic icon and TUI remains free to select terminal presentation.
It consumes runtime snapshots and commands and does not construct Player, Engine, decoder, backend, or library-source state.

Frontend composition creates concrete providers and transfers them to `AppRuntime`.
Frontend widgets and platform endpoints issue runtime/UIModel commands and render observations; they do not calculate succession or call audio control-plane objects directly.

### Application access surfaces

`AppRuntime` exposes `playback()` and `playbackSequence()` as separate runtime services rather than one public playback facade.
`PlaybackCommandSurface` deliberately composes both: transport controls address `PlaybackService`, while next/previous and playback modes address `PlaybackSequenceService`; starting the focused selection returns through the `AppRuntime` coordinator into sequence launch.

`PlaybackService` also exposes lower-level direct start and prepare operations.
A direct transport start does not construct a source lease, projection, or cursor: an existing sequence may adopt the observation when its source context matches, may deactivate when it does not match, and no new sequence exists when transport starts alone.
Composite session persistence rejects cursor/transport current-subject mismatch instead of inventing missing succession context.

## Boundaries and dependency direction

- Frontends and UIModel depend toward runtime; runtime depends toward core audio and library facilities; neither dependency reverses.
- `PlaybackSequenceService` may depend on views, sources, the library, notifications, timers, and the private sequence collaboration surface of `PlaybackService`.
- `PlaybackService` may depend on library reads, notifications, and its exclusively owned Player, but not on UIModel or frontend types.
- `PlaybackSessionPersistence` coordinates public runtime state and persistence; `ConfigStore` serializes its values but does not own their meaning.
- Player and Engine depend only on core audio, media, utility, async-executor, and platform-provider abstractions; they do not depend on runtime types.
- Concrete providers are constructed at platform composition roots and transferred through core audio interfaces.
- A library identity is resolved into a filesystem playback input before crossing into core audio; audio callbacks return opaque item and generation identities rather than application TrackIds.
- Dedicated event, decoder, render, and device threads never access sequence, transport-service, UIModel, frontend, notification, or storage state.
- Normal view-based playback starts at the sequence boundary; direct PlaybackService starts are transport-only operations and cannot claim a new live-source context.

## Data and control flow

Playback uses four cross-boundary protocols plus a separate realtime data plane.

### Explicit-start protocol

A view-based start is prepared before it replaces the active succession context:

```text
ViewId + TrackId
  -> Sequence captures launch context from ViewService
  -> Sequence constructs a candidate leased source/projection/cursor
  -> PlaybackService resolves runtime metadata and PlaybackInput
  -> Player/Engine stage decoder, source, route, and initial position
  -> Engine commit returns item generation + cancellation barrier
  -> PlaybackService publishes the accepted current request
  -> Sequence installs the candidate and prepares its successor
```

The candidate session remains separate during staging.
A failure before Engine accepts the start leaves the previous sequence session and transport authority in place; the exact publication and rollback contract belongs in the playback behavior specification that will own explicit starts.

### Prepared-transition protocol

Prepared-next crosses three different identity domains:

```text
Sequence: TrackId + maintained projection anchor
       -> PlaybackService: resolved request + prepared-token registry
       -> Player/Engine: opaque item id + playback generation
       -> natural advance event
       -> PlaybackService maps accepted item/generation to runtime request
       -> Sequence correlates the returned token with live succession state
```

Sequence remains the successor authority while Engine owns whether a prepared audio transition can execute gaplessly or must drain.
Generation barriers retire callbacks from superseded audio starts; prepared tokens correlate application intent without exposing TrackId to Engine.
Prepared audio is a best-effort cross-domain commitment, not a transaction with library membership.

The correlation values have separate owners and meanings:

| Evidence | Owner and purpose | Not evidence of |
|---|---|---|
| `TrackId` and `ListId` | Runtime library subject and source context. | An accepted or still-current audio item. |
| `PreparedNextToken` | PlaybackService/Sequence correlation for one prepared application intent. | Engine generation or current list membership. |
| `Engine::PlaybackItemId` | Opaque audio item identity allocated at the runtime/audio bridge and echoed by Engine. | Library identity or succession policy. |
| Playback generation and cancellation barrier | Engine/Player proof that older audio callbacks can no longer win. | Projection revision or persistence revision. |
| Session persistence revision | Persistence proof that one captured durable intent was acknowledged. | Audio callback or prepared-transition freshness. |

No layer infers one form of evidence from another.

### Observation protocol

All lower asynchronous observations converge on the callback executor before they become application state:

```text
backend / decoder / render signal
  -> Engine event queue and generation checks
  -> Player callback gate
  -> runtime callback executor
  -> PlaybackService snapshot and typed events
  -> PlaybackSequenceService when succession context is required
  -> UIModel and frontend observers
```

Engine callbacks are observational until Player accepts their generation and the callback-executor task runs.
Synchronous Engine control returns do not require an asynchronous callback to make their immediate result visible to the caller.
The realtime signal ring is bounded, but the non-realtime Engine queue and one-task-per-Player-callback path currently have no end-to-end capacity/coalescing contract; [RFC 0028](../rfc/0028-bounded-audio-observation-delivery.md) proposes typed event classes and one bounded drain.

### Persistence protocol

Save and restore coordinate the two application authorities:

```text
Sequence persistence intent + transport persistence intent
  -> PlaybackSessionPersistence captures both snapshots
  -> reject mismatched current subjects
  -> ConfigStore candidate save with composite revision acknowledgement

stored payload
  -> validate against storage and format
  -> prepare source/projection/cursor candidate
  -> prepare deferred transport state
  -> install cursor inside transport restore publication
  -> publish one coherent restored application state
```

The persistence coordinator owns validation and installation order; it does not serialize projection rows, prepared audio, thread generations, or other regenerable execution state.

### Realtime PCM data plane

Decoded bytes flow independently from the application observation path:

```text
DecoderSession
  -> StreamingSource decode thread
  -> PCM ring buffer
  -> Backend render callback
  -> platform audio device
```

Engine control prepares and retires the objects visible to this path.
The render callback does not call runtime code; a natural transition emits only a bounded realtime signal that the Engine event worker later turns into control-plane state and callbacks.

`StreamingSource` expresses preroll and steady-state high watermarks as capacity-bounded byte targets.
Its sole PCM producer requests another decoded block only while below the target and while the ring has predictive headroom for the previous nonempty block.
Synchronous preroll and seek filling use the same policy as the later decode worker; a larger-than-predicted block retains the cancellable partial-write fallback.
This producer-side policy adds no lock, notification, or accounting operation to the realtime consumer.

Engine seek establishes exclusive PCM reset access in three steps.
Backend `stop()` first closes render admission and waits for the active render cycle to return, complete `status()` snapshots participate in Engine control serialization while observing buffered duration, and `StreamingSource::seek()` stops and joins its decode producer before resetting the ring in constant time.

## Execution domains

The detailed process-wide rules belong to [runtime execution architecture](runtime-execution.md).
Playback refines them as follows:

| Domain | Authoritative owner | Access and synchronization | Exit toward another domain |
|---|---|---|---|
| Runtime callback executor | Sequence, PlaybackService, PlaybackSessionPersistence, Player application state | Executor affinity; no lower callback mutates these objects inline. | Values and commands enter Engine; observations publish to UIModel/frontends. |
| Async runtime timer/worker | Persistence delays and cancellable scheduling | Stop tokens and weak/shared lifetime guards; it resumes on the callback executor before touching playback state. | Scheduled checkpoint intent returns to the callback domain. |
| Engine control domain | Engine transport, route attachment, timeline, and synchronized snapshots | Thread-tolerant commands and complete status snapshots are serialized by Engine control/state synchronization; scalar state-only queries use the narrower state synchronization. | Commands affect Backend/StreamingSource; notifications are queued to the event worker. |
| Engine event worker | Ordered backend/source events and realtime transition signals | One owned worker, synchronized event queue, bounded realtime signal ring. | Engine callbacks enter the Player callback gate. |
| StreamingSource decode thread | Decoder progress and PCM production for one source | Owned `jthread`, decoder/error synchronization, stop tokens, capacity-bounded byte targets, and producer-confined block headroom over the PCM ring. | PCM enters the render plane; errors enqueue toward Engine. |
| Backend render/device domains | Native output, render cursor, and provider discovery | Backend-specific synchronization; realtime render avoids Engine control locks and unbounded work. | Non-realtime events enqueue toward Engine or provider callbacks toward Player. |

Player is the intentional bridge between the callback-executor domain and the thread-tolerant Engine.
Engine is the intentional bridge between serialized control and the dedicated event, decode, and render domains.
The Engine control domain is a synchronization domain, not a dedicated control thread: public control calls execute synchronously on their caller and are serialized internally.
Through Player the caller is normally the runtime callback executor, so current track opening, source construction, preroll, and route activation performed during staging remain on that synchronous call path; only later event delivery, streaming decode, and rendering use dedicated threads.
Playback-session timers sleep outside the callback executor, but snapshot construction and the one-shot `ConfigStore` save run synchronously after resuming on it.

Runtime playback signals are delivered synchronously within the callback domain.
PlaybackService batches outbound events and rejects ordinary transport mutation while a batch is draining; sequence-owned collaboration uses a narrow grant, while Sequence separately guards accepted starts, session installation, restore, and observer publication.
These guards contain reentrant commands but do not create a third transaction executor.

## Structural constraints

- One `AppRuntime` owns one interactive playback stack; `CoreRuntime` alone owns none.
- Succession, application transport, and session persistence have distinct owners and collaborate through narrow runtime ports.
- The current audio subject may continue after its source membership changes; the audio execution domain is not a live-list replica.
- Frontends supply identities and commands, never a materialized playback queue or Engine item.
- A playback session owns its source/projection lifetime and cannot borrow the originating view's lifetime.
- Player exclusively owns providers and Engine; PlaybackService exclusively owns Player.
- Runtime-visible state and public playback observations are accepted on the callback executor.
- Engine callbacks carry generation evidence; application prepared transitions additionally carry a token that can be correlated without leaking application identity into core audio.
- Realtime render performs no UI, storage, notification, list policy, blocking join, or general application work.
- Backend `stop()` is the render-quiescence boundary: it is called outside the render domain, prevents another render cycle, and waits for the active cycle and its render notifications to return.
- StreamingSource has one PCM producer at a time; Engine quiesces render and buffered-duration observation, then seek stops and joins the producer before resetting the ring and its predictive block-headroom state.
- Decoder and backend implementations remain replaceable without changing succession or session-persistence policy.
- Persisted listening intent contains application semantics, not transient projections, prepared handles, audio generations, or thread state.
- A coherent durable session requires matching sequence and transport current subjects; persistence reports an invariant failure rather than persisting split generations.
- Synchronous callback-executor work includes current audio preflight and configuration save, so it remains part of interactive latency even though steady-state decode and render are off-thread.

## Failure, cancellation, and lifetime boundaries

Core audio classifies execution failures, while Engine and Player own quiescence of the failing audio activity; PlaybackService translates accepted lower failures into application transport state and public observations.
PlaybackSequenceService owns recovery only when choosing another source member requires cursor context.
The persistence coordinator owns malformed-session rejection, durable retry, and restore normalization.

Cancellation evidence is scoped to its owner.
Async persistence work uses stop tokens and lifetime guards; StreamingSource owns decode/seek stop sources; Engine and Player use playback generations and cancellation barriers to reject stale callbacks and prepared transitions.
None of these mechanisms substitutes for another layer's lifetime proof.

Shutdown proceeds from callback producers toward their dependencies:

1. `PlaybackSessionPersistence::shutdown` cancels scheduled work, releases subscriptions, and performs its final checkpoint policy while sequence, transport, ConfigStore, and async runtime remain alive.
2. `PlaybackService::shutdown` closes runtime publication and asks Player to quiesce lower activity while sequence and other runtime consumers still exist.
3. Player closes its callback gate, unsubscribes provider observations, shuts down provider event sources, then shuts down Engine while provider-owned dependencies still exist.
4. Engine stops and joins its event worker, retires render state, closes the backend, and releases track sources; StreamingSource destruction stops and joins decode workers.
5. Sequence, transport, view, workspace, and persistence objects are destroyed before `CoreRuntime` stops its worker pool and releases the callback executor, library, and notifications.

A callback already executing may call supported control operations but cannot synchronously destroy or shut down its emitting owner.
Queued Player callbacks become no-ops after the gate closes, and every dedicated producer is stopped and joined before the state it may address is destroyed.

## Implementation map

- [`AppRuntime.cpp`](../../app/runtime/AppRuntime.cpp) composes sequence, transport, persistence, view, workspace, and teardown ownership.
- [`PlaybackSequenceService`](../../app/include/ao/rt/PlaybackSequenceService.h) and [`PlaybackCursorSession`](../../app/runtime/playback/PlaybackCursorSession.h) own succession composition and its live source/projection lifetime.
- [`PlaybackCursor`](../../app/runtime/playback/PlaybackCursor.h), [`ProjectionAnchor`](../../app/runtime/playback/ProjectionAnchor.h), [`ShuffleHistory`](../../app/runtime/playback/ShuffleHistory.h), and [`PreparedNextRegistry`](../../app/runtime/playback/PreparedNextRegistry.h) divide pure/session policy.
- [`PlaybackService`](../../app/include/ao/rt/PlaybackService.h) owns application transport, runtime-to-audio translation, and accepted observation publication.
- [`PlaybackSessionPersistence`](../../app/runtime/PlaybackSessionPersistence.h) coordinates the composite durable session lifecycle.
- [`PlaybackCommandSurface`](../../app/include/ao/uimodel/playback/command/PlaybackCommandSurface.h) is the reusable UIModel command boundary.
- [`OutputDeviceViewModel`](../../app/include/ao/uimodel/playback/output/OutputDeviceViewModel.h) and [`PresentationTextCatalog`](../../app/include/ao/uimodel/presentation/PresentationTextCatalog.h) own shared output-device presentation.
- [`Player`](../../include/ao/audio/Player.h) owns providers, Engine, route/quality state, and callback marshalling.
- [`Engine`](../../include/ao/audio/Engine.h), [`TrackSession`](../../lib/audio/detail/TrackSession.h), and [`StreamingSource`](../../include/ao/audio/StreamingSource.h) own audio execution and source construction; [`PcmRingBuffer`](../../include/ao/audio/PcmRingBuffer.h) and [`StreamingBufferPolicy`](../../lib/audio/detail/StreamingBufferPolicy.h) own bounded PCM capacity and producer admission.
- [`BackendProvider`](../../include/ao/audio/BackendProvider.h) and [`Backend`](../../include/ao/audio/Backend.h) define the platform output boundary.
- [`AudioBackendBootstrap.cpp`](../../app/linux-gtk/platform/AudioBackendBootstrap.cpp) and [`app/tui/AudioBackendBootstrap.cpp`](../../app/tui/AudioBackendBootstrap.cpp) construct concrete providers at frontend composition roots.

## Test map

- [`AppRuntimeTest.cpp`](../../test/unit/runtime/AppRuntimeTest.cpp) protects interactive composition and service lifetime.
- [`PlaybackSequenceServiceTest.cpp`](../../test/unit/runtime/PlaybackSequenceServiceTest.cpp), [`PlaybackCursorModelTest.cpp`](../../test/unit/runtime/playback/PlaybackCursorModelTest.cpp), and [`ProjectionAnchorTest.cpp`](../../test/unit/runtime/playback/ProjectionAnchorTest.cpp) protect succession ownership and the pure/session boundary.
- [`PlaybackServiceControlTest.cpp`](../../test/unit/runtime/PlaybackServiceControlTest.cpp), [`PlaybackServiceOutputTest.cpp`](../../test/unit/runtime/PlaybackServiceOutputTest.cpp), and [`PlaybackServiceTokenTest.cpp`](../../test/unit/runtime/PlaybackServiceTokenTest.cpp) protect application transport and cross-domain identity.
- [`PlaybackSessionTest.cpp`](../../test/unit/runtime/PlaybackSessionTest.cpp) and [`PlaybackSessionRevisionTest.cpp`](../../test/unit/runtime/playback/PlaybackSessionRevisionTest.cpp) protect composite persistence and restore coordination.
- [`PlaybackCommandSurfaceTest.cpp`](../../test/unit/uimodel/playback/command/PlaybackCommandSurfaceTest.cpp) protects the UIModel/runtime command boundary.
- [`PlayerTest.cpp`](../../test/unit/audio/PlayerTest.cpp) protects Player ownership, provider/Engine composition, and callback marshalling.
- [`EngineConcurrencyTest.cpp`](../../test/unit/audio/EngineConcurrencyTest.cpp), [`EngineCallbackTest.cpp`](../../test/unit/audio/EngineCallbackTest.cpp), [`EngineGaplessTest.cpp`](../../test/unit/audio/EngineGaplessTest.cpp), and [`EngineBackendSwapTest.cpp`](../../test/unit/audio/EngineBackendSwapTest.cpp) protect Engine control, status/seek serialization, event, transition, and backend boundaries.
- [`StreamingSourceTest.cpp`](../../test/unit/audio/StreamingSourceTest.cpp), [`PcmRingBufferTest.cpp`](../../test/unit/audio/PcmRingBufferTest.cpp), and [`StreamingBufferPolicyTest.cpp`](../../test/unit/audio/detail/StreamingBufferPolicyTest.cpp) protect decode-worker ownership, bounded PCM admission, seek, constant-time reset reuse, and teardown.
- [`AudioBackendBootstrapTest.cpp`](../../test/unit/linux-gtk/platform/AudioBackendBootstrapTest.cpp) protects provider construction at the GTK composition boundary.

## Related documents

- [System architecture](system-overview.md)
- [Runtime execution architecture](runtime-execution.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Encoded media architecture](encoded-media.md)
- [Library architecture](library.md)
- [Resource delivery architecture](resource-delivery.md)
- [Presentation text catalog reference](../reference/presentation/text-catalog.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Presentation architecture](presentation.md)
- [Workspace architecture](workspace.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Audio quality architecture](audio-quality.md)
- [Audio quality analysis specification](../spec/playback/quality-analysis.md)
- [Audio quality surface reference](../reference/playback/quality-surface.md)
- [Playback succession cursor specification](../spec/playback/cursor.md)
- [Decoder session specification](../spec/playback/decoder-session.md)
- [Audio execution and concurrency specification](../spec/playback/audio-execution.md)
- [Playback session persistence specification](../spec/playback/session-persistence.md) and [state reference](../reference/playback/session-state.md)
- [RFC 0028: bounded audio observation delivery](../rfc/0028-bounded-audio-observation-delivery.md)
