---
id: playback.application-commit
type: spec
status: current
domain: playback
summary: Defines PlaybackService intent admission, supersession, coherent commits, revision publication, observer reentrancy, transient-event correlation, and shutdown.
---
# Playback application commits

## Scope

This specification defines the application-level playback transaction boundary:
public command admission, intent ordering and supersession, coherent snapshot
commits, revision assignment, observer reentrancy, transient-event correlation,
and shutdown. The exact public C++ types belong to the
[application-boundary reference](../../reference/playback/application-boundary.md).

Succession policy belongs to the [playback cursor](cursor.md), audio execution
and generation fences belong to [audio execution](audio-execution.md), and
durable restore policy belongs to [session persistence](session-persistence.md).

## Code boundary

This contract belongs to `PlaybackService` in the application runtime layer.
Its private implementation owns the commit state and serial intent pump while
borrowing the runtime-internal `PlaybackTransport` and `PlaybackSuccession`
owners. Those collaborators retain their focused state and policy; neither is
a public mutation or publication authority.

## Terminology

- **Intent**: one accepted public playback command or synchronous restore
  operation.
- **Intent generation**: monotonic internal admission identity used to reject a
  queued intent superseded by a newer invalidating intent.
- **Command id**: public correlation identity assigned at intent admission and
  attached to a failure while its originating command remains known.
- **Logical commit**: one executor-affine transition from the previous coherent
  snapshot to the next coherent snapshot.
- **External settlement**: accepted lower-layer state arriving from Player,
  Engine, provider, or natural-advance callbacks rather than a public command.
- **Publication turn**: delivery of one snapshot or transient event to all
  currently connected observers.

## Invariants

- `PlaybackService` is the only public playback mutation and publication
  authority.
- A logical commit that changes semantic content advances `PlaybackRevision`
  exactly once and publishes exactly one snapshot. A no-op advances and
  publishes nothing.
- Revision state advances even when no snapshot observer is connected.
- A published snapshot is assembled only after lower transport and succession
  mutations for that commit have settled.
- While succession is active, transport and succession identify the same
  current track and source.
- A command issued from a public playback observer never executes on that
  observer stack. It enters a later executor turn.
- Once an accepted intent is waiting for a deferred drain, every later public
  command remains behind that intent. No synchronous command may overtake the
  pending FIFO.
- Stop, replacement start, next, previous, restore, output-route change, clear,
  and shutdown invalidate an older queued start/navigation intent. Orthogonal
  volume, mute, pause, resume, seek, shuffle, and repeat intents are not dropped
  merely because a newer invalidating intent exists.
- Elapsed clock drift alone is not semantic content and advances no application
  or position revision.
- Shutdown closes admission and makes every already-deferred service task safe
  to drop before transport callback producers are quiesced.

## State model

The service retains the last committed snapshot, application/position/final-seek
revision counters, intent and command-id counters, the newest invalidating
generation, at most one scheduled intent drain, a FIFO of admitted deferred
intents, pending transient events for the active commit, lower-layer
subscriptions, and a weak deferred-task gate.

Mutable state is confined to the runtime callback executor. The pump creates no
worker or mutex. Detached audio preparation is a later RFC 0005 stage; current
intent execution remains synchronous once its executor turn begins.

## Commands and transitions

### Admission and execution

Every accepted public command receives an intent generation and command id.
Outside a commit or publication, and while no deferred intent is pending, it
executes synchronously and its lower signals are folded into that command's
commit. During a commit or public publication, or while the FIFO already has a
pending drain or entry, it joins the tail. One deferred task consumes one queued
intent at a time.

`startFromView` returns a synchronous admission result. If it is admitted from
an observer, lower validation happens when the queued intent executes; a later
failure is reported through `PlaybackFailureEvent` with the command id.
Session restore keeps its call-level result on `AppRuntime`, so a restore is
rejected while a commit, publication, or deferred intent is active instead of
being queued against a caller-owned result or overtaking the FIFO.

### Supersession

A queued start, next, or previous intent records the invalidating generation
current at admission. A newer replacement start, navigation, restore, stop,
clear, output-route change, or shutdown raises that generation. When the older
intent reaches the head of the queue, the service discards it before it can
touch either lower owner.

Quick orthogonal intents remain FIFO entries and execute normally. This Stage 3
rule establishes deterministic intent priority; Stage 4 extends the same
generation to preparation already running off the callback executor.

### Explicit start and navigation

Succession prepares a candidate cursor session and transport stages an audio
start without replacing the current public snapshot. Transport then commits
its accepted current request without publishing start/now-playing observations
back into succession. Succession installs the candidate and prepares its next
track. The service observes the settled lower state and publishes one revision.

Next and previous use the same silent explicit-start collaboration. Natural
advance remains driven by accepted Engine/Player evidence; its lower
observations are coalesced into one external-settlement commit.

### Quick mutations and external settlement

Pause, resume, final seek, volume, mute, shuffle, repeat, stop, clear, and output
selection execute within a service commit. Lower signals mark that commit dirty;
the service composes only after the command returns. A final seek advances both
position identities, while subject replacement or same-subject restart advances
the position identity only. Silent lower navigation and terminal-idle settlement
are accounted before external publication, including fallback natural advance.

Provider, readiness, quality, prepared-next, and natural-advance observations
that arrive outside a public intent schedule one deferred publication. Multiple
lower signals already accepted in the same executor turn compose one coherent
external-settlement revision.

### Restore

Restore validates and prepares its cursor candidate before mutation. Transport
installs the deferred idle request, offset, volume, and mute without emitting a
partial restore publication. Succession then installs the prepared session.
`PlaybackService` composes and publishes the combined result when the enclosing
synchronous restore intent returns. Every restore that installs a candidate
establishes a new position anchor and therefore publishes a combined snapshot,
including a repeated or same-subject idle restore whose only changed public
value is its restored offset. A successful no-session lookup installs nothing
and creates no anchor.
Restore advances `PlaybackPositionRevision` only; it never advances
`PlaybackFinalSeekRevision`, because restoring a durable anchor is not a final
seek command.

No transport `beforePublish` callback or lower-owner restore/publication guard
participates in coherence.

## Observation and transient events

Snapshot publication is exception-contained: one observer failure does not
prevent remaining observers or the commit from completing. Commands requested
by snapshot, failure, seek-preview, or reveal observers join the intent queue and
cannot change the value currently being delivered.

Failures and seek previews carry the application revision current at delivery.
A failure observed during an admitted command also carries that command id.
Seek previews do not commit snapshot content. Pending transient events produced
inside a command are delivered after its snapshot commit, so their revision
refers to the resulting current state.

## Failure and cancellation

A result-bearing command rejected because admission is closed returns its
`Result` error and creates no revision; void commands issued after shutdown are
ignored. If synchronous lower validation rejects an admitted command, that call
returns the lower error. A queued command was already admitted; if its later
lower validation rejects it, the service emits at most one correlated failure
and leaves the snapshot unchanged.

An unexpected exception is an invariant fault rather than a typed playback
failure. Before propagating it to the synchronous caller or deferred executor,
the service settles any already-observed mutation, closes commit bookkeeping,
and attempts to schedule the next queued intent. A failed deferred-task
admission rolls back its scheduled marker so a later admission can retry the
visible FIFO.

Supersession is cancellation by intent identity, not by audio generation.
Prepared-next tokens, Engine item ids, audio cancellation barriers, and durable
session revisions remain independent evidence owned by their respective layers.

## Shutdown

`AppRuntime` first shuts down session persistence. It then closes
`PlaybackService`, which rejects new commands, invalidates queued generations,
clears queued intents and lower subscriptions, and revokes deferred service
tasks. `PlaybackBootstrap` subsequently shuts down transport and Player callback
producers while succession and the remaining runtime graph are still alive.

## Implementation map

- [`PlaybackService.cpp`](../../../app/runtime/playback/PlaybackService.cpp)
  owns intent admission, commit composition, revision publication, transient
  correlation, and the deferred-task gate.
- [`PlaybackTransport.cpp`](../../../app/runtime/playback/PlaybackTransport.cpp)
  owns silent explicit-start and restore installation plus lower event delivery.
- [`PlaybackSuccession.cpp`](../../../app/runtime/playback/PlaybackSuccession.cpp)
  owns candidate session installation and succession policy.
- [`AppRuntime.cpp`](../../../app/runtime/AppRuntime.cpp) owns synchronous restore
  admission and teardown order.

## Test map

- [`PlaybackServiceTest.cpp`](../../../test/unit/runtime/PlaybackServiceTest.cpp)
  proves coherent revisions, no-subscriber advancement, observer deferral,
  FIFO preservation, supersession, failure command identity, exception drain
  continuation, scheduler rejection recovery, and pending-intent lifetime.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp)
  proves one restored snapshot, same-subject offset publication, repeated-restore
  baselines, backlog/reentrant restore rejection, and deferred nested commands.
- [`PlaybackTransportTokenTest.cpp`](../../../test/unit/runtime/PlaybackTransportTokenTest.cpp)
  proves internal publication completes despite observer exceptions.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Playback application boundary reference](../../reference/playback/application-boundary.md)
- [RFC 0005](../../rfc/0005-coherent-playback-boundary.md)
