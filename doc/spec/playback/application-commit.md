---
id: playback.application-commit
type: spec
status: current
domain: playback
summary: Defines PlaybackService command ordering, coherent snapshot commits, observer reentrancy, supersession, and shutdown.
---
# Playback application commits

## Scope

This specification defines the application-level playback boundary: public
command ordering, coherent snapshot commits, observer reentrancy, queued-command
supersession, lower-layer settlement, and shutdown. The exact public C++ types
belong to the
[application-boundary reference](../../reference/playback/application-boundary.md).

Succession policy belongs to the [playback cursor](cursor.md), audio execution
and generation fences belong to [audio execution](audio-execution.md), and
durable restore policy belongs to [session persistence](session-persistence.md).

## Code boundary

This behavior belongs to `PlaybackService` in the **application runtime** layer
of the [system architecture](../../architecture/system-overview.md), under the
[playback architecture](../../architecture/playback.md). Public declarations
live in `app/include/ao/rt/playback/`; the private implementation in
`app/runtime/playback/PlaybackService.cpp` owns commit state and the serial
command queue while borrowing the runtime-internal `PlaybackTransport` and
`PlaybackSuccession` owners.

## Terminology

- **Logical commit** is one callback-executor-affine transition from the last
  coherent snapshot to a newly composed snapshot.
- **External settlement** is accepted lower-layer state arriving from Player,
  Engine, a provider, or natural-advance callbacks rather than a public command.
- **Publication turn** is delivery of one snapshot or transient event to all
  currently connected observers.
- **Command generation** is the internal ordering value used to discard an old
  queued start or navigation command after a newer invalidating command.
- **Pending view start** is the private candidate succession session and request
  retained after synchronous validation while audio preparation runs on a worker.

## Invariants

- A logical commit publishes exactly one snapshot when semantic content changes
  and publishes nothing for a no-op.
- A snapshot is assembled only after matching transport and succession changes
  have settled.
- While succession is active, transport and succession identify the same current
  track and source.
- A command issued by a playback observer never executes on that observer stack.
- Once the deferred queue is non-empty, later public commands join its tail; no
  synchronous command overtakes it.
- Stop, replacement start, next, previous, restore, output-route change, clear,
  and shutdown supersede older queued start/navigation commands. Orthogonal
  volume, mute, pause, resume, seek, shuffle, and repeat commands remain FIFO.
- Elapsed clock drift alone is not semantic content and causes no publication.
- Shutdown closes command admission and makes deferred service tasks safe to
  drop before transport callback producers are quiesced.

## State model

The service retains the last committed snapshot, position and final-seek
counters, command generations, one FIFO, at most one scheduled drain, lower
subscriptions, pending seek previews and reveal requests, commit/publication
depth, and one weak deferred-task gate. Succession additionally retains at most
one pending view start and one pending lookahead successor identity.

This state is confined to the runtime callback executor. The command queue adds
no worker or mutex. Player owns the cancellable `async::Runtime` tasks used for
view-start and gapless-lookahead decoder/source preparation; workers carry
isolated audio values and do not own application state.

## Commands and transitions

### Admission and execution

Outside a commit or publication, and with no queued backlog, a command executes
synchronously. During a commit or publication, or while a backlog exists, it is
appended to the FIFO. One deferred task consumes one command and schedules the
next drain after settlement.

`startFromView` reports synchronous view, membership, request, readiness, and
worker-task admission. Success does not mean that the decoder opened or that a
new current subject was installed. When called by an observer it initially
reports successful command-queue admission; it has no separate public completion
token. Session restore keeps its call-level result on `AppRuntime` and is
rejected while a commit, publication, or backlog is active.

### Supersession

A queued start, next, or previous command records its generation. A newer
replacement start, navigation, restore, stop, clear, output-route change, or
shutdown raises the invalidating generation. An older positioning command is
discarded before touching either lower owner when it reaches the queue head.

### Explicit start and navigation

Succession synchronously validates and constructs a cursor candidate, then
admits audio preparation without replacing public state. Decoder open, format
negotiation, initial seek, and preroll run on an async worker while the previous
succession session, transport subject, snapshot, and audio generation remain
current.
The lower preparing observation may mark an in-progress commit, but it does not
replace the current public snapshot with a `Preparing` transport snapshot.

Completion returns to the callback executor. Succession revalidates its pending
candidate, live source, and membership; transport re-resolves the track and
compares `PlaybackInput`; Engine revalidates playback generation, route, and
start context. Player task-handle cancellation and the callback-resumption
stop-token checkpoint prevent a superseded worker completion from reaching
those acceptance checks. Only then may Engine adopt the source, transport
commit the start, succession install the candidate and request best-effort
lookahead, and the service publish the settled snapshot. Format evidence is
revalidated for gapless lookahead, where compatibility with the current stream
determines whether Engine may arm a splice; it is not separate explicit-start
evidence.
Before installation, succession reapplies the current repeat and shuffle modes
to the candidate so policy changes accepted during worker preparation are not
lost.

Unprepared Next and Previous retain their synchronous navigation start path.

Natural advance remains driven by accepted Engine/Player evidence. Its lower
observations are coalesced into one external-settlement publication.

### Quick mutations and external settlement

Pause, resume, final seek, volume, mute, shuffle, repeat, stop, clear, and output
selection execute inside a service commit. Lower signals mark the commit changed;
composition waits until the command returns.

A final seek advances both position identities. Subject replacement,
same-subject restart, terminal idle, and successful restore advance only the
position identity. Provider, readiness, quality, prepared-next, and
natural-advance observations received outside a command schedule one deferred
publication; observations accepted in the same executor turn coalesce.

### Restore

Restore validates and prepares its cursor candidate before mutation. Transport
installs the deferred idle request, offset, volume, and mute without lower
publication; succession then installs the prepared session. `PlaybackService`
publishes the combined state when the synchronous restore command returns.

Every installed restore creates a new position anchor, including a repeated or
same-subject idle restore. A missing stored session installs and publishes
nothing. Restore never advances `PlaybackFinalSeekRevision`.

## Observation

Snapshot publication is exception-contained: one observer failure does not
prevent remaining observers or commit completion. Observer-issued commands join
the queue.

Seek previews carry only their elapsed position and do not alter the snapshot.
Reveal requests carry their navigation fields. Events produced within a command
are delivered after snapshot settlement.

## Failure and cancellation

A result-bearing command rejected because admission is closed returns
`InvalidState`; void commands after shutdown are ignored. Immediate lower
validation errors return to the caller. A queued command has no later public
result channel; playback execution failures continue through the internal
recovery and notification owners.

An admitted view-start decoder failure is asynchronous: it publishes the
existing track-open notification and leaves the previous session and snapshot
unchanged. A newer start, stop, navigation command, final seek, output change,
clear, source or membership mutation, or shutdown invalidates the pending
start. A semantic acceptance veto completes once as `Conflict`, allowing start
or lookahead completion to clear the matching pending state deterministically;
task cancellation, replacement, or teardown may suppress completion entirely.
Engine reports captured playback, route, or start-context evidence that became
stale as `Conflict`; transport treats that result as supersession and does not
publish a track-open or route-activation notification.
Lookahead preparation is best-effort: completion clears only its matching
pending bookkeeping, and stale or `Conflict` completion has no semantic effect.
The [playback cursor](cursor.md) owns current preparation-failure reroll and
boundary recovery policy.

An unexpected exception is an invariant fault. Before propagating it, the
service settles observed state, closes commit bookkeeping, and attempts to keep
the queue drain live. If scheduling a drain throws, its marker is rolled back so
a later admission can retry.

Command supersession is independent from prepared-next tokens, Engine item ids,
audio cancellation barriers, and persisted session state.

## Shutdown

`AppRuntime` first shuts down session persistence. It then closes
`PlaybackService`, which rejects new commands, invalidates and clears queued
commands, disconnects lower observations, and revokes deferred service tasks.
`PlaybackBootstrap` subsequently shuts down transport and Player callback
producers while succession and the remaining runtime graph are alive.

## Implementation map

- [`PlaybackService.cpp`](../../../app/runtime/playback/PlaybackService.cpp)
  owns command ordering, commit composition, publication, and the deferred-task
  gate.
- [`PlaybackTransport.cpp`](../../../app/runtime/playback/PlaybackTransport.cpp)
  owns silent explicit-start and restore installation plus lower event delivery.
- [`PlaybackSuccession.cpp`](../../../app/runtime/playback/PlaybackSuccession.cpp)
  owns candidate session installation and succession policy.
- [`AppRuntime.cpp`](../../../app/runtime/AppRuntime.cpp) owns synchronous restore
  admission and teardown order.

## Test map

- [`PlaybackServiceTest.cpp`](../../../test/unit/runtime/PlaybackServiceTest.cpp)
  protects coherent publication, observer deferral, FIFO ordering, supersession,
  exception recovery, scheduler rejection, and queued-command lifetime.
- [`PlaybackSuccessionTest.cpp`](../../../test/unit/runtime/PlaybackSuccessionTest.cpp)
  protects asynchronous admission, failure isolation, candidate installation,
  and prepared-next correlation.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp)
  protects coherent restore, repeated-restore baselines, backlog/reentrant restore
  rejection, and deferred nested commands.
- [`PlaybackTransportTokenTest.cpp`](../../../test/unit/runtime/PlaybackTransportTokenTest.cpp)
  protects internal event delivery despite observer exceptions.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Playback application boundary reference](../../reference/playback/application-boundary.md)
