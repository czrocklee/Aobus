---
id: rfc.0005.coherent-playback-boundary
type: rfc
status: accepted
domain: playback
summary: Proposes one compositional playback boundary, coherent application transactions, and non-blocking preparation and persistence.
depends-on: none
---
# RFC 0005: Coherent playback application boundary

## Disposition

Accepted on 2026-07-19 with the open questions resolved below.
Implementation proceeds in the staged order of the compatibility and migration section; execution detail lives in the local plan tree.
Stages 1 through 3 are complete: consumers use the public `PlaybackService`,
the renamed `PlaybackTransport` and `PlaybackSuccession` owners are
runtime-internal, and the service's private serial intent/commit pump is the
sole application revision authority. Observer commands are deferred and
superseded by intent generation, while the former cross-owner publication,
privilege, installation, and restore guards have been removed. The resulting
behavior is owned by the
[playback application commit specification](../spec/playback/application-commit.md).

The serialized asynchronous persistence stage was not adopted.
On 2026-07-21, playback-session persistence was deliberately simplified to synchronous best-effort saves on discrete intent and natural lifecycle boundaries, without a playback-specific dirty revision or retry scheduler.
The current [playback session persistence specification](../spec/playback/session-persistence.md) owns that implemented behavior; the persistence section below remains proposal history rather than pending implementation direction.

## Problem

At acceptance, the playback implementation had sound separation between application semantics and Core audio execution, but its application boundary exposed the internal split instead of containing it.
[`AppRuntime`](../../app/include/ao/rt/AppRuntime.h) returned the former transport `PlaybackService` (now internal `PlaybackTransport`) and `PlaybackSequenceService` (now internal `PlaybackSuccession`) independently.
UIModel, GTK, TUI, MPRIS, workspace restore, and session persistence therefore assemble transport state and succession state themselves or subscribe to only one half of a logical playback change.

This creates six related problems.

### Normal callers can bypass the succession authority

The former transport service publicly exposed direct `playTrack`, `play`, `stagePlayback`, `commitPlayback`, and prepared-next operations even though a normal view-based start must also establish a source lease, live projection, cursor, and succession policy.
The production view-start path went through the former sequence service, but the type system did not make that route authoritative.

The mismatch is observable rather than theoretical.
[`PlaybackSessionTest`](../../test/unit/runtime/PlaybackSessionTest.cpp) starts one track through the sequence service, starts a different track through the transport service, and verifies that session save fails with `InvalidState` because the cursor and transport subjects disagree.
Persistence correctly refuses to invent missing context, but the public application API permits the invalid composite state that it later rejects.

### One logical transition is coordinated by local guards

Accepted starts, natural advances, restore, outbound publication, and sequence installation cross two service-owned states.
The implementation at acceptance protected these crossings with several independent mechanisms, including `AcceptanceTransaction`, `ObserverPublicationScope`, `SessionInstallationTransaction`, `RestoreTransaction`, `SuccessionMutationGrantScope`, `OutboundEventBatchScope`, and a restore `beforePublish` callback.

Each mechanism addresses a real local reentrancy or ordering hazard.
Together they are evidence that the application transaction boundary is missing: no single owner defines when a combined playback revision is accepted, visible, persistable, or superseded.

### Observers have no coherent application snapshot

The former transport and sequence `state()` methods had independent publication paths.
A consumer that needs now-playing, transport, cursor, next/previous availability, shuffle, and repeat reads and subscribes across both services.
The callback executor serializes execution, but serialization alone does not identify which pair of snapshots belongs to one committed application transition.

### Quality and route observations lack application correlation

The [audio quality architecture](../architecture/audio-quality.md) correctly rejects stale provider callbacks with internal Player generations before publishing runtime state.
However, the former public transport `QualityChanged` payload contained only quality and readiness.
It carries no application revision or selected-output identity even though route settlement may publish several intermediate results.

A consumer can reread the latest mutable playback state, but it cannot prove that an event, output selection, readiness value, and quality graph describe the same accepted application snapshot.
Internal audio generations prevent old callbacks from winning; they do not provide the application-level correlation required by UIModel and frontend observers.

### Interactive callback work includes blocking preparation and storage

The [current playback architecture](../architecture/playback.md) records that `Player` is callback-executor-affine while `Engine` is thread-tolerant.
The present start path calls `Player::stagePlayback()` synchronously from that executor.
[`Engine::stagePlayback`](../../lib/audio/Engine.cpp) holds the Engine control domain while it opens a decoder, negotiates the route format, seeks, constructs a `StreamingSource`, fills a 500 ms preroll, and starts its decode worker.
Slow filesystem, decoder, or media work therefore delays unrelated callback-executor work.

The existing prepared-start type cannot simply be moved to an arbitrary worker.
`Player::stagePlayback()` enforces executor affinity, and `Engine::PreparedPlaybackStart` unregisters itself through a raw owner pointer during destruction.
Its safety depends on Engine lifetime and its captured start context.

Playback-session scheduling sleeps away from the callback executor, but it resumes there before snapshot capture and the one-shot `ConfigStore::save()` operation, including candidate cloning, YAML emission, and atomic file replacement.
`ConfigStore` owns one cached ryml document and has no concurrent-access contract; the playback session store may also be the workspace store.
Moving only one caller's save to a worker while other clients continue accessing the same store would introduce a race or lost-update boundary instead of fixing latency.

### Responsibilities and evidence leak across domains

The former transport service combined transport, library resolution, route and quality state, notifications, prepared-item correlation, restore state, persistence snapshots, and reveal-track navigation signals.
The navigation signal is presentation policy, not playback transport.

Playback transitions also carry several legitimate but distinct identities: `TrackId`/`ListId`, `PreparedNextToken`, `Engine::PlaybackItemId`, playback generation and cancellation barrier, and persistence revision.
Local code can accidentally treat proximity between these values as equivalence even though none proves the freshness or meaning of another.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Expose one application-level playback boundary from `AppRuntime` while keeping transport, succession, persistence, and Core audio as distinct internal responsibilities.
- Make `PlaybackService` the only public mutation authority for explicit start, natural advance, stop, restore, and prepared-next acceptance.
- Publish one immutable application snapshot and revision for every accepted logical transition.
- Correlate every accepted output, readiness, and quality change with the application revision and selected output it describes.
- Make cursor/transport subject disagreement unrepresentable in normal public playback commands.
- Preserve the separate meanings of application identity, prepared-intent correlation, audio identity, audio generation, and durable revision.
- Remove decoder open, format negotiation, seek, preroll, and uncommitted prepared-source disposal from the callback executor.
- Remove YAML emission and filesystem replacement from the callback executor without allowing concurrent access to `ConfigStore` state.
- Preserve Core audio isolation, realtime constraints, output routing, gapless preparation, cancellation barriers, and existing succession behavior.
- Move reveal-track navigation out of the transport owner.
- Provide an incremental implementation path with testable boundaries after each phase.

## Non-goals

- Merge `PlaybackSuccession`, `PlaybackTransport`, `PlaybackSessionPersistence`, `Player`, and `Engine` into one implementation class.
- Move shuffle, repeat, live-source recovery, or cursor policy into Core audio.
- Make `Engine` aware of library, view, list, or persistence identities.
- Replace the Engine event worker, decoder workers, backend render model, or PCM data plane.
- Change the current playback-session schema merely to introduce the application boundary.
- Define resampling, decoder format, or output-quality behavior already owned by playback specifications and audio design documents.
- Change audio-quality classification, finding severity, or presentation verdict policy.
- Turn UIModel into the playback state authority or move frontend presentation state into runtime.
- Preserve source compatibility for the current dual-service public surface; the repository has no compatibility constraint for this internal API.

## Proposed design

### Target ownership

`AppRuntime` owns one compositional `PlaybackService` object.
`PlaybackService` exposes narrow roles; it does not implement every playback responsibility.

```text
AppRuntime
  |-- PlaybackService                  public composition boundary
  |     |-- snapshot()                 current coherent value
  |     |-- PlaybackCommands           public mutation port
  |     |-- PlaybackEvents             public subscription port
  |     `-- private intent/commit pump sole application commit mechanism
  |           |-- PlaybackSuccession   source lease, projection, cursor, policy
  |           |-- PlaybackTransport    now-playing, transport, output, quality
  |           |     `-- Player         Core-audio bridge
  |           |           `-- Engine -> source -> backend
  |           `-- SessionPersistence   durable composite intent
  |-- ViewService
  |-- WorkspaceService
  `-- navigation/presentation routing
```

`AppRuntime::playback()` returns `PlaybackService&` and no longer exposes `playbackSequence()`.
Composition roots may obtain a private bootstrap role for provider registration and shutdown, but frontend and UIModel code cannot obtain the transport or succession implementations.

Consumers receive the narrowest stable role they need:

- `PlaybackCommands` accepts user and platform intents.
- `PlaybackService::snapshot()` returns the current coherent value.
- `PlaybackEvents` owns snapshot-publication and transient-event subscriptions.
- UIModel may compose the value, command, and event surfaces into reusable command and presentation models.
- `AppRuntime` lifecycle code alone uses bootstrap, restore, and shutdown collaboration.

This is a composition and access point, not a god object.
Adding a new internal playback responsibility does not automatically add methods to `PlaybackService`; a method belongs there only when it is part of a public command, state, or lifecycle role.

### Coherent state and publication

`PlaybackService::snapshot()` exposes one immutable value with a monotonically increasing application revision:

```cpp
struct PlaybackSnapshot final
{
  PlaybackRevision revision;
  PlaybackTransportSnapshot transport;
  PlaybackSuccessionSnapshot succession;
  PlaybackPreparationSnapshot preparation;
};
```

The exact field inventory remains a later specification decision.
The structural contract is that one snapshot contains all runtime playback state needed to interpret a transition.
The transport portion includes selected output, output readiness, and the accepted quality snapshot so those values cannot advance as unrelated public states.

For every published snapshot:

- an active succession subject and the transport now-playing subject are the same `TrackId` and source context;
- an idle transport may retain a coherent succession subject only as deferred restored intent; an idle transport with no subject has no active succession session;
- a prepared successor is either correlated to the current succession revision or absent;
- the application revision advances once after the complete logical commit, not once per internal collaborator;
- observers receive the new snapshot only after all application invariants hold.

Specialized events such as failure, seek progress, and non-state diagnostics may remain typed event streams.
Every state-changing event carries the application revision of its self-contained state or identifies the revisioned snapshot it announces, so consumers never infer pairing from callback order.

### Quality and route observation

An accepted quality update is a playback-state mutation even when it is an intermediate result while a route settles.
`PlaybackService` captures selected output, readiness, and quality together, advances `PlaybackRevision`, and publishes one `PlaybackSnapshot` for that accepted state.

A specialized quality notification may remain for efficient subscribers, but it is not an independent state authority.
Its conceptual payload is:

```cpp
struct PlaybackQualityObservation final
{
  PlaybackRevision revision;
  OutputDeviceSelection output;
  bool ready;
  PlaybackQualitySnapshot quality;
};
```

The notification is self-contained and matches the snapshot with the same revision.
A consumer that has already observed a newer snapshot discards an older notification rather than combining it with current output state.
Player playback/route generations remain internal evidence for rejecting stale provider callbacks and are not substituted for `PlaybackRevision`.

Route settlement may therefore publish several monotonically revisioned quality snapshots.
A stale provider callback rejected below the service commit boundary publishes no application revision.

### Command serialization and reentrancy

`PlaybackService` is callback-executor-affine and its private implementation owns a serial intent pump.
The pump is an implementation mechanism of the public service, not a second domain class or public role.
Public commands do not call internal collaborators recursively through observer callbacks.

Each accepted intent receives a `PlaybackIntentGeneration`.
Long-running start and restore operations may suspend while preparation runs, but a newer start, stop, shutdown, route change, or restore can cancel or invalidate their generation.
Only the current generation may enter the application commit step.

Commands requested synchronously by an observer are posted to the next service turn.
They never mutate the snapshot currently being published.
This single rule replaces service-specific publication depth, privilege, and restore guards at the public boundary.
Internal assertions may remain where they document a collaborator invariant, but they are not the transaction mechanism.

Commands that can fail before acceptance return an asynchronous result or completion token.
Small transport commands may still complete within one executor turn, but their public contract does not depend on inline observer reentrancy.

### Explicit-start transaction

A normal track start is one service-owned transaction:

1. On the callback executor, capture a view launch description and construct a candidate succession session without replacing the current session.
2. Resolve the candidate `TrackId` into an immutable audio-preparation specification.
3. Start cancellable audio preparation outside the callback executor.
4. On return to the callback executor, reject stale intent, source, route, or shutdown generations.
5. Ask Engine to adopt the prepared session and commit the audio generation.
6. After a successful Engine receipt, perform a no-fail swap of the prepared transport and succession application state.
7. Publish one new `PlaybackSnapshot` and then schedule prepared-next work for that revision.

Failure before Engine acceptance leaves the previous application snapshot and audio generation current.
After Engine accepts, application installation must contain no fallible work; all library resolution, allocation needed by the candidate, and invariant validation happen earlier.
Queued lower-layer callbacks remain subject to the Engine/Player generation barriers before `PlaybackService` accepts them.

There is no generic public `playTrack(TrackId, ListId)` that silently creates transport-only state.
If Aobus later needs detached playback, it requires an explicit `PlaybackOrigin` variant with defined succession and persistence semantics rather than an omitted cursor side effect.

### Natural transition and prepared-next transaction

Succession remains the only owner of next-track policy.
It prepares an application successor against a specific application revision and source projection.
Transport maps that successor to Core audio without exposing library identity below `Player`.

An accepted natural transition returns a typed internal envelope to the service commit boundary.
`PlaybackService` verifies each item in its own domain, advances succession, updates transport, and publishes once.
Stale, cleared, or source-invalidated preparation cannot be accepted merely because an Engine item id still matches.

The design intentionally does not create one universal playback token.
It carries distinct evidence together:

| Evidence | Authority | Required use |
|---|---|---|
| `TrackId` and source context | Succession/runtime library | Identify the application subject and live source. |
| `PreparedNextToken` | Application preparation registry | Correlate one prepared successor intent. |
| `Engine::PlaybackItemId` | Runtime/audio bridge | Correlate one opaque Core audio item. |
| Playback generation and cancellation barrier | Engine/Player | Reject callbacks and starts from superseded audio execution. |
| `PlaybackRevision` | PlaybackService | Correlate public snapshots and application commits. |
| Persistence revision | Session persistence | Acknowledge one durable snapshot write. |

The envelope proves that each authority agreed to the same transition; no field is derived from or substituted for another.

### Worker-safe audio preparation

Audio preparation becomes a separate Core audio operation rather than an off-thread call to the current `Player::stagePlayback()`.

The callback/Engine control path first captures a short-lived immutable `AudioPreparationSpec` containing the playback input, initial offset, selected route description, decoder construction contract, and the Engine start-context revision to revalidate.
It must not expose mutable backend, Player, or Engine objects to the worker.

A preparation worker opens and negotiates the decoder, applies the initial seek, constructs the streaming source, and fills preroll into a detached `PreparedTrackSession`.
That handle owns all uncommitted decoder/source lifetime and has no raw pointer back to Engine.
Its decoder factory and error sink must have an explicit worker-safe lifetime contract.

Engine adoption is a short control-domain operation.
It verifies that Engine is alive and that device, profile, route, base playback generation, and start-context revision still match the captured specification.
It then assigns source/playback generations, registers the prepared state, and commits or returns a stale-context result.
A route change may retry preparation under the new route when the originating intent is still current.

Cancellation and failed adoption dispose of an uncommitted `PreparedTrackSession` on a worker because destroying a `StreamingSource` may stop and join its decode thread.
Shutdown closes the service intent gate, cancels preparations, waits for their detached ownership to quiesce, and only then destroys Player and Engine.

This split removes decoder I/O and preroll from both the callback executor and long Engine control-lock sections while preserving Engine as the execution authority.

### Serialized asynchronous persistence

`PlaybackSessionPersistence` captures a durable session value only from a committed `PlaybackSnapshot` on the callback executor.
The captured value carries both its application revision and a persistence revision.

Persistence uses a serialized configuration writer with exclusive mutation rights for a given `ConfigStore` instance:

```text
callback executor
  -> immutable group update + persistence revision
  -> serialized configuration writer
       -> load/apply all ordered group updates
       -> emit YAML
       -> atomic file replacement
  -> callback-executor acknowledgement
```

All clients sharing the same underlying `ConfigStore` must use that writer; playback cannot move the store to a worker while workspace code continues to access it directly.
The writer coalesces superseded playback revisions without reordering updates to other groups.
Successful acknowledgement clears dirty state only through the acknowledged revision.
Failure preserves dirty state and reports the latest durable error on the callback executor.

The initial implementation may use a playback-exclusive store to limit migration scope.
When the fallback workspace store is used, introducing the serialized writer is a prerequisite rather than an optional optimization.
YAML parsing needed for startup restore may remain a startup operation, but no autosave file operation runs on the interactive callback executor.

### Navigation boundary

Reveal-current-track intent moves out of `PlaybackTransport`.
Playback state exposes the current subject and source context; a UIModel or application navigation coordinator translates a reveal command into `ViewService`/`WorkspaceService` navigation.
Playback does not publish frontend navigation requests.

### Code-boundary result

The proposed boundary remains aligned with the [system architecture](../architecture/system-overview.md):

- application runtime owns playback coordination, succession, transport adaptation, and durable intent;
- Core audio owns decoding, routing, audio generations, source lifetime, and realtime execution;
- UIModel owns reusable command availability and presentation adaptation;
- frontends own native controls, navigation presentation, and platform endpoints.

The proposal changes access and commit ownership inside application runtime.
It does not redraw the top-level Core/runtime/UIModel/frontend layers.

## Alternatives

### Keep both services public and document the calling convention

This preserves the smallest code diff, but callers can still create a state that persistence rejects.
Documentation cannot provide atomic publication, one revision, cancellation ownership, or a uniform reentrancy rule.

### Add only a thin forwarding object over the existing services

A forwarding object that leaves the internal services publishing independently improves discoverability but not correctness.
It also risks becoming a third API beside the two old ones.
The selected design removes public access to collaborators and moves commit authority into `PlaybackService`.

### Merge all playback code into one service

One class would hide the split at the cost of erasing useful authority boundaries and concentrating unrelated policy, storage, and Core-audio bridging.
The selected design unifies the transaction boundary while retaining narrow collaborators.

### Call the current Engine or Player stage method on a worker

Calling `Player::stagePlayback()` off-thread violates its executor-affinity contract.
Calling `Engine::stagePlayback()` directly from a worker still yields an Engine-owned prepared handle whose destructor reaches a raw owner pointer, keeps blocking work inside the Engine control lock, and couples task lifetime to Engine shutdown.
The selected detached preparation handle makes worker ownership and adoption explicit.

### Give Engine a dedicated actor thread

An Engine actor could move synchronous work away from the callback executor, but it would serialize decoder I/O with unrelated route and transport control unless it introduced a second preparation pool anyway.
It would also replace the current thread-tolerant control model without addressing application snapshot coherence.
The selected design changes only the blocking preparation phase.

### Use one universal transition id

Collapsing application identity, audio identity, generations, and durability into one number makes logs look simpler but weakens proof boundaries.
Those values have different allocation, invalidation, and lifetime rules.
The selected transition envelope retains typed evidence from every authority.

### Move only playback's `ConfigStore::save()` to a worker

The one-shot save safely isolates one candidate, but the store's cached document can still be shared by playback and workspace groups and remains executor-confined.
Moving only playback save requires unsafe concurrent store access or allows a callback-executor workspace save to race from an older complete-document snapshot.
The selected serialized writer owns every shared-store save, ordering, emission, and file replacement together.

## Compatibility and migration

No persisted schema change is required for this RFC.
Existing valid playback sessions continue to restore, and malformed or cursor/transport-mismatched payloads remain rejected.

Implementation proceeds in boundary-preserving phases:

1. Introduce the combined revisioned snapshot, `PlaybackCommands`, and `PlaybackEvents`, migrate all production consumers, and make `AppRuntime::playback()` the sole public playback accessor.
2. Name that public business boundary `PlaybackService`; rename and relocate the legacy owners as internal `PlaybackTransport` and `PlaybackSuccession`, remove public low-level start/prepare access, and reserve bootstrap access for composition and lifecycle.
3. Move explicit start, natural advance, restore, stop, and publication into the private `PlaybackService` intent pump, then remove redundant cross-service transaction guards.
4. Introduce detached Core audio preparation, adoption revalidation, worker disposal, and shutdown quiescence.
5. Introduce the serialized configuration writer and migrate every client of a shared store before enabling asynchronous playback autosave on it.
6. Move reveal navigation to the application/UIModel navigation boundary and narrow the transport collaborator.
7. Reassess the internal owners after coordination is stable and split them only if their remaining responsibilities require smaller boundaries.

During phases one and two, adapters may preserve existing synchronous behavior internally, but no new production caller may depend on the legacy services.
Tests may retain direct collaborator fixtures for unit coverage after those types become implementation details.

## Validation

Acceptance requires evidence at the boundary, behavior, concurrency, and frontend levels.

### Boundary checks

- Repository checks prove that production code outside runtime implementation cannot include or name the internal transport and succession services.
- `AppRuntime` exposes one `PlaybackService` object and no independent succession accessor.
- Public command and state roles contain no Engine, decoder, backend, or persistence implementation types.
- Reveal-track events no longer originate from playback transport.

### Transaction checks

- Explicit start failure at library resolution, decoder open, preroll, stale route, and Engine adoption leaves the previous combined snapshot current.
- Successful explicit start publishes exactly one new application revision containing matching succession and transport subjects.
- No public command can reproduce the current cursor/transport mismatch save test.
- Natural advance accepts only matching source revision, prepared token, Engine item id, and audio generation evidence.
- Restore publishes no intermediate cursor-only or transport-only state.
- Every accepted quality event carries a revision, output identity, readiness, and quality payload matching one published snapshot.
- Multiple route-settling quality updates advance monotonically without allowing an older provider generation to publish a later application revision.
- Switching output never presents quality evidence from the previous output as if it belonged to the replacement selection.
- Observer-initiated commands execute on a later service turn and never alter the snapshot being delivered.
- Stop, replacement start, route change, and shutdown deterministically invalidate an in-flight preparation.

### Responsiveness and concurrency checks

- A controlled decoder that blocks open or preroll does not prevent callback-executor heartbeat, stop, output, or unrelated UI work.
- Cancelling or superseding preparation releases its decoder/source on a worker and does not join a decode thread on the callback executor.
- Route change between capture and adoption rejects stale preparation and either retries under policy or reports a deterministic result.
- Engine shutdown with preparation in flight produces no callback after quiescence and no prepared-handle access to destroyed Engine state.
- A controlled slow or failing atomic write does not block callback-executor progress.
- Coalesced persistence writes acknowledge only the revision actually made durable and preserve ordered updates to other configuration groups.
- Playback concurrency contracts pass under the repository ThreadSanitizer workflow described by [concurrency and sanitizers](../development/test/concurrency-and-sanitizer.md).

### Regression checks

- Existing cursor, shuffle, repeat, live-source mutation, prepared-next, restore, route, quality, decoder, and Engine cancellation tests remain behavior oracles.
- Audio-quality analyzer and verdict behavior remains owned by the current quality specification; this RFC changes only application snapshot correlation and publication.
- GTK controls, MPRIS, TUI, and UIModel consume the new command/state roles without rebuilding succession policy.
- Documentation checks pass after every promotion step, and the completed implementation passes the normal full `./ao check` gate.

## Resolved questions

The following decisions closed acceptance; none changes the selected ownership and transaction model.

- The serialized configuration writer is a general runtime-layer type from the start, but its first migration wave covers only the two genuinely shared store instances (the GTK global configuration store and the TUI/workspace store that doubles as the playback-session store).
  Stores backed by their own file, such as the shell layout store, keep synchronous saves and are not part of this proposal.
- A route or output change invalidates an in-flight preparation; the service intent pump retries preparation exactly once under the new route while the originating intent is still current, and otherwise completes the intent with a deterministic stale-route error.
  This is a fixed single retry, not a general retry-policy framework.
- Public commands do not adopt a uniform asynchronous completion surface.
  Restore, save, and discard keep call-level results; explicit start returns a synchronous admission result and reports its asynchronous outcome through the revisioned failure event once worker preparation is introduced; small transport commands return no completion token.
- No supported frontend requires a detached-playback origin.
  The CLI has no playback stack and every frontend starts playback from a captured view context, so all starts require a source context and no transport-only public start exists.
- Beyond the combined snapshot, the retained typed event streams are playback failure and seek preview; the specialized quality notification is not an independent state authority and carries the same application revision as the snapshot it matches.

## Promotion plan

Implementation updates the [playback architecture](../architecture/playback.md) from the former dual-service model to the `PlaybackService` commit-authority model and records the accepted rationale in current documentation.
Update the [interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) where `PlaybackService` changes `AppRuntime` composition, startup restore coordination, or teardown ownership.
The [persistence and managed-state architecture](../architecture/persistence-and-managed-state.md) is updated when the serialized writer replaces the current executor-confined shared-store boundary.

Behavioral contracts discovered during implementation move into focused playback specifications for application transactions, snapshot publication, cancellation, and persistence.
Exact public command/state fields and any persistence-schema changes move into reference documents rather than this RFC.
The current [playback succession cursor](../spec/playback/cursor.md) specification remains the behavioral authority unless the implemented service commit boundary changes one of its contracts.
The current [audio quality architecture](../architecture/audio-quality.md), [quality analysis specification](../spec/playback/quality-analysis.md), and [quality surface reference](../reference/playback/quality-surface.md) remain authoritative for evidence, classification, and exact values; implementation updates them only where the new revisioned application publication surface changes their boundary.
Current detailed evidence lives in the [playback session persistence](../spec/playback/session-persistence.md), [playback session state](../reference/playback/session-state.md), and [audio execution and concurrency](../spec/playback/audio-execution.md) contracts.

Contributor guidance gains a short playback-boundary map only if implementation work demonstrates a recurring navigation need; it does not duplicate the architecture or specifications.
When all validation criteria pass, this RFC becomes `implemented` and links to the resulting current documents.
