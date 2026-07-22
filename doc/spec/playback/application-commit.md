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
depth, and one weak deferred-task gate.

This state is confined to the runtime callback executor. The command queue adds
no worker or mutex. Audio preparation remains synchronous once a command begins
executing; [RFC 0033](../../rfc/0033-nonblocking-playback-preparation.md) proposes
isolating decoder/source preparation on the existing worker pool.

## Commands and transitions

### Admission and execution

Outside a commit or publication, and with no queued backlog, a command executes
synchronously. During a commit or publication, or while a backlog exists, it is
appended to the FIFO. One deferred task consumes one command and schedules the
next drain after settlement.

`startFromView` returns the lower result when it executes immediately. When
called by an observer it reports successful queue admission; it has no separate
completion token. Session restore keeps its call-level result on `AppRuntime`
and is rejected while a commit, publication, or backlog is active.

### Supersession

A queued start, next, or previous command records its generation. A newer
replacement start, navigation, restore, stop, clear, output-route change, or
shutdown raises the invalidating generation. An older positioning command is
discarded before touching either lower owner when it reaches the queue head.

### Explicit start and navigation

Succession prepares a cursor candidate and transport stages the audio start
without replacing public state. Transport silently installs the accepted
request, succession installs the candidate and prepares its successor, then the
service composes the settled snapshot. Next and previous use the same path.

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
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp)
  protects coherent restore, repeated-restore baselines, backlog/reentrant restore
  rejection, and deferred nested commands.
- [`PlaybackTransportTokenTest.cpp`](../../../test/unit/runtime/PlaybackTransportTokenTest.cpp)
  protects internal event delivery despite observer exceptions.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Playback application boundary reference](../../reference/playback/application-boundary.md)
- [RFC 0033: non-blocking playback preparation](../../rfc/0033-nonblocking-playback-preparation.md)
