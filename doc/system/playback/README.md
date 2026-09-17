---
id: architecture.playback
---
# Playback architecture

## Find the contract for your change

- [Command admission, coherent snapshots, and observer reentrancy](application-commit.md)
- [Next/previous, repeat, shuffle, prepared next, and recovery](cursor.md)
- [Audio execution, generation fences, backend lifetime, and shutdown](audio-execution.md)
- [Decoder sessions](decoder-session.md) and [decoder errors](decoder-error.md)
- [PCM representation and lossless output](pcm-format.md)
- [Session restore and checkpoint behavior](session-persistence.md) and the [serialized state format](../../reference/playback/session-state.md)
- [Quality ownership](quality.md), [analysis behavior](quality-analysis.md), and [quality values](quality-values.md)

## Scope

This page explains how interactive playback is divided across application runtime, Core audio, UIModel, and frontends. It identifies the state authorities, dependency direction, execution domains, and lifetime order. The linked topic contracts own detailed command, cursor, decoder, PCM, quality, and persistence behavior.

Interactive playback is composed by `AppRuntime`, which owns a `CoreRuntime` beneath the playback graph. A `CoreRuntime` used by the batch CLI does not by itself create the interactive playback stack.

## Ownership model

```text
frontend widgets and platform adapters
                 |
                 v
       UIModel playback models
                 |
                 v
              AppRuntime
                 |
                 +-- PlaybackService       public command, snapshot, and event boundary
                 |      |
                 |      +-- PlaybackSuccession
                 |      |      `-- PlaybackCursorSession
                 |      |
                 |      `-- PlaybackTransport
                 |             `-- ao::audio::Player
                 |                    `-- ao::audio::Engine
                 |
                 +-- PlaybackSessionPersistence
                 |
                 `-- ViewService + TrackSourceCache

Player / Engine
  +-- BackendProvider -> Backend -> native output
  `-- decoder session -> StreamingSource -> PCM ring -> render callback
```

The graph crosses the layers in the [system architecture](../overview.md):

| Area | Layer | Boundary |
|---|---|---|
| Succession, transport, and persistence | Application runtime | [`app/include/ao/rt/`](../../../app/include/ao/rt/) with private owners under [`app/runtime/`](../../../app/runtime/) |
| Commands and display adaptation | UIModel | [`app/include/ao/uimodel/playback/`](../../../app/include/ao/uimodel/playback/) |
| Player, Engine, decoder, PCM, and backend interfaces | Core libraries | [`include/ao/audio/`](../../../include/ao/audio/) |
| Concrete decoding, buffering, routing, and native output | Core libraries | [`lib/audio/`](../../../lib/audio/) |
| Widgets and platform lifecycle | Frontends | [GTK, TUI, WinUI, and AppKit application trees](../frontend/README.md) |

Playback is not one state machine shared by all layers. Application runtime owns listening semantics; Core audio owns execution. The boundary between them carries explicit starts, prepared transitions, output commands, opaque audio identities, and observations.

## State authorities

### PlaybackService

`PlaybackService` is the only public application playback object exposed by `AppRuntime`. It provides a coherent snapshot plus narrow command and event roles. It serializes observer-issued commands, coordinates the two internal owners, and publishes at most one coherent snapshot after a logical mutation settles.

Consumers cannot obtain `PlaybackTransport` or `PlaybackSuccession` from `AppRuntime`. Public starts enter through a view-based command so a transport subject cannot be created without matching succession context. The exact admission, supersession, borrowing, and publication rules are in [playback application commits](application-commit.md).

### PlaybackSuccession

`PlaybackSuccession` owns where playback came from and where it may move next. A `PlaybackCursorSession` pins the source lease and owns a detached live projection, cursor anchor, shuffle history, and prepared-next correlation for one playback conversation. The originating view is launch input, not a lifetime dependency, and the frontend never supplies a materialized queue.

Membership edits govern future succession but do not interrupt audio that is already current. Repeat, shuffle, gap anchors, source invalidation, prepared-next races, and recovery walking are defined by the [cursor specification](cursor.md).

### PlaybackTransport

`PlaybackTransport` owns the current application subject and translates runtime requests into Player operations. It retains launch-time metadata and runtime occurrence identity, adapts pause/resume/stop/seek and output commands, and publishes executor-affine transport observations.

It does not decide membership, ordering, repeat, shuffle, or source recovery. Core audio receives a filesystem playback input and opaque item identity, not `TrackId`, `ListId`, view, or persistence semantics.

### PlaybackSessionPersistence

`PlaybackSessionPersistence` coordinates the composite restorable state across succession and transport. It validates stored input, prepares candidates before installation, observes coherent state, and schedules natural-boundary or debounced saves. It is not a third live-state authority.

The payload contains application listening intent, not projection rows, prepared handles, output devices, decoder state, audio generations, or thread state. See [session persistence](session-persistence.md) and the [version 4 format](../../reference/playback/session-state.md).

### Player and Engine

`ao::audio::Player` owns providers and one Engine. It selects output, merges Engine and provider route evidence, analyzes delivery quality, and marshals lower observations onto the runtime callback executor. Its application-facing state is executor-affine.

`ao::audio::Engine` owns the synchronized audio timeline, current and lookahead sources, backend attachment, playback and route generations, render correlation, and the event worker. It treats item ids as opaque and has no library, view, succession, notification, or persistence policy.

`StreamingSource` owns one decoder session, its decode worker, the PCM ring, seek cancellation, and source-error handoff. Backends own native output resources and call only the narrow realtime render surface. Detailed synchronization, render-quiescence, generation, and provider-lifetime guarantees belong to [audio execution](audio-execution.md).

### UIModel and frontends

UIModel turns runtime snapshots and commands into reusable availability and presentation state. It owns shared output selection presentation, quality formatting, and Soul aura/motion policy, but it does not construct or control Player, Engine, decoders, backends, sources, or persistence.

Frontend composition obtains the platform provider set from Core audio and transfers ownership into `AppRuntime`. Linux prefers PipeWire then ALSA; Windows supplies WASAPI; macOS supplies Core Audio. Frontends issue runtime/UIModel commands and adapt presentation to their toolkit; they do not calculate succession or quality severity.

## Cross-boundary protocols

### Explicit start

A view start captures a launch description and constructs a candidate cursor session before changing active state. Player performs decoder inspection and optional optimistic decoder preparation on runtime workers. Acceptance returns to the callback executor and revalidates the source, request, playback generation, route, and start context before Engine commits.

The old application and audio state remains authoritative while preparation is pending. Commit is the destructive boundary: it retires the old render target, opens the backend, validates the returned PCM mode, and either reuses the exact matching prepared source or creates the final decoder in that mode. The [application commit](application-commit.md), [audio execution](audio-execution.md), and [decoder session](decoder-session.md) specifications own failure and cancellation outcomes.

### Prepared transition

Prepared next crosses separate identity domains:

```text
TrackId and maintained projection anchor
  -> application PreparedNextToken
  -> opaque Engine item id and playback generation
  -> accepted natural-advance observation
  -> token correlation back in succession
```

No layer infers one identity from another. Succession remains the successor authority; Engine decides whether compatible prepared audio can splice or must drain. Prepared audio is best effort and does not prove current list membership or future decoder success.

### Observation

```text
backend / decoder / render signal
  -> Engine event worker and generation checks
  -> Player callback gate
  -> runtime callback executor
  -> PlaybackTransport and PlaybackSuccession
  -> PlaybackService coherent snapshot
  -> UIModel and frontend observers
```

Lower callbacks are observational until their generation is accepted and the callback-executor task runs. Realtime render never calls runtime, UI, storage, notification, or list policy.

### Persistence

Persistence captures matching succession and transport state from a coherent application boundary. Restore validates and prepares a source/cursor candidate, installs deferred transport without lower publication, installs succession, and then publishes one combined state. A mismatch is an invariant failure rather than a payload assembled from different playback generations.

## Execution and thread boundaries

| Domain | Owns | Boundary rule |
|---|---|---|
| Runtime callback executor | PlaybackService, succession, transport, persistence, Player application state | Runtime-visible mutations, public observations, and Player public state are executor-affine. |
| Async runtime workers/timers | Decoder inspection/preparation, lookahead preparation, persistence delays | Workers hold isolated values; accepted results return through cancellation-checked executor hops. Decoder calls are not forcibly interruptible. |
| Engine control synchronization | Timeline, backend attachment, route and playback generations | Public control calls run on their callers and are serialized internally; this is not a dedicated control thread. |
| Engine event worker | Backend/source events and realtime transition signals | Applies accepted events and originates Engine callbacks off producer stacks. |
| StreamingSource decode worker | PCM production for one source | One producer writes a bounded ring; seek stops and joins it before reset. |
| Backend render/device domains | Native output and PCM consumption | Realtime work is bounded and does not take application or Engine control locks. |

Player intentionally bridges the callback executor and thread-tolerant Engine. Engine bridges synchronous control with event, decode, and render domains. Cancellation evidence is local to its owner: service command generations, Player task handles, Engine playback/route generations, prepared-next tokens, and persistence task generations are not interchangeable.

## Dependency and safety constraints

- Frontends and UIModel depend toward runtime; runtime depends toward Core audio and library facilities; these dependencies do not reverse.
- PlaybackSuccession may use views, source leases, projections, library reads, notifications, timers, and the private transport collaboration surface.
- PlaybackTransport exclusively owns Player; Player exclusively owns provider facades and Engine.
- Provider subscriptions, callbacks, workers, and returned backends retain only narrow shared state needed to outlive a provider facade safely.
- Engine and dedicated audio threads never access runtime sequence state, UIModel, frontends, storage, or library identities.
- Backend `stop()` is the render-quiescence boundary; `close()` revokes the render target.
- One StreamingSource producer exists at a time. Render and buffered-duration observation are quiescent before its ring is reset.
- Persisted playback state contains application semantics only; regenerated projections, prepared state, output route, and execution generations are excluded.

## Shutdown order

Shutdown removes callback producers before the state they can address:

1. Session persistence cancels scheduled work, releases subscriptions, and performs a final checkpoint only when its lifecycle admits one.
2. `PlaybackService` closes command admission, drops pending work, disconnects lower observations, and revokes deferred callbacks.
3. Playback bootstrap asks transport and Player to quiesce lower activity while runtime collaborators still exist.
4. Player closes its callback gate, cancels preparation tasks, releases provider observations, completes provider shutdown, and shuts down Engine.
5. Engine stops its event worker, quiesces render, closes the backend, and releases sources; source destruction stops decode workers.
6. Runtime playback objects are destroyed before `CoreRuntime` stops the shared worker pool and releases its executor, library, and notification services.

A decoder or filesystem call already executing on a worker may extend final worker-pool shutdown, but cancellation prevents its late value from committing. Provider callback self-destruction and shared shutdown completion have the stronger provider-specific rules in [audio execution](audio-execution.md).

## Primary implementation boundaries

- [`PlaybackService`](../../../app/include/ao/rt/playback/PlaybackService.h), [`PlaybackSuccession`](../../../app/runtime/playback/PlaybackSuccession.h), and [`PlaybackTransport`](../../../app/runtime/playback/PlaybackTransport.h)
- [`PlaybackSessionPersistence`](../../../app/runtime/PlaybackSessionPersistence.h)
- [`Player`](../../../include/ao/audio/Player.h), [`Engine`](../../../include/ao/audio/Engine.h), [`DecoderSession`](../../../include/ao/audio/DecoderSession.h), and [`BackendProvider`](../../../include/ao/audio/BackendProvider.h)
- [`PlaybackActions`](../../../app/include/ao/uimodel/playback/command/PlaybackActions.h), [`OutputDeviceViewModel`](../../../app/include/ao/uimodel/playback/output/OutputDeviceViewModel.h), and [`AudioQualityFormatter`](../../../app/include/ao/uimodel/playback/quality/AudioQualityFormatter.h)

Focused tests live beside these owners under `test/unit/runtime/`, `test/unit/audio/`, and `test/unit/uimodel/playback/`. The linked topic pages identify the tests for each exact contract.

## Related architecture

- [System architecture](../overview.md)
- [Runtime execution architecture](../execution/README.md)
- [Failure and reporting architecture](../failure/README.md)
- [Encoded media architecture](../media/README.md)
- [Library architecture](../library/structure.md)
- [Persistence and managed-state architecture](../persistence/README.md)
- [Presentation architecture](../presentation/README.md)
- [Interactive session lifecycle architecture](../session-lifecycle.md)
