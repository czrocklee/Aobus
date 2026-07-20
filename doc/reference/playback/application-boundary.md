---
id: playback.application-boundary
type: reference
status: current
domain: playback
summary: Enumerates the coherent PlaybackService surface — its committed snapshot, event subscriptions, and commands exposed by AppRuntime.
---
# Playback application boundary

## Scope and version

This reference owns the exact public types of the coherent playback boundary introduced by [RFC 0005](../../rfc/0005-coherent-playback-boundary.md): `PlaybackService`, its `PlaybackCommands` and `PlaybackEvents` roles, and the immutable `PlaybackSnapshot` plus revisioned event payloads.

It is the RFC 0005 Stage 3 surface.
`PlaybackService` is the public commit authority over the runtime-internal `PlaybackTransport` and `PlaybackSuccession` owners, while `AppRuntime` exposes only the public service to consumers.
Those internal headers live under `app/runtime/playback/` and repository guardrails reject their use by public runtime headers, UIModel, and frontends.
The service's private implementation owns intent ordering and revision commits; there is no separate coordinator type or public snapshot-source object.

## Code boundary

- Layer: application runtime, per the [system architecture](../../architecture/system-overview.md) and [playback architecture](../../architecture/playback.md).
- Public headers: `app/include/ao/rt/playback/` (`PlaybackService.h`, `PlaybackCommands.h`, `PlaybackEvents.h`, `PlaybackSnapshot.h`).
- Implementation: `app/runtime/playback/PlaybackService.cpp`.
- Accessor: `ao::rt::AppRuntime::playback()` returns `PlaybackService&`.
- Internal owners and bootstrap: `app/runtime/playback/PlaybackTransport.h`, `PlaybackSuccession.h`, and `PlaybackBootstrap.h`; none is part of this surface.

## Surface

### `PlaybackService`

| Member | Type | Meaning |
|---|---|---|
| `snapshot()` | `PlaybackSnapshot const&` | Borrows the last boundary-committed coherent state. The borrowed view remains stable until the next snapshot publication or service destruction. |
| `commands()` | `PlaybackCommands&` | The mutation port. |
| `events()` | `PlaybackEvents&` | The publication and transient-event subscription port. |

### `PlaybackRevision`

| Field | Type | Default | Meaning |
|---|---|---|---|
| `value` | `std::uint64_t` | `0` | Monotonic identity of one accepted application transition. Totally ordered; never derived from an Engine item id, audio generation, or persistence revision. |

### Position identities

`PlaybackPositionRevision` and `PlaybackFinalSeekRevision` each wrap a comparable `std::uint64_t value` whose default is `0`.

- `PlaybackPositionRevision` identifies the playback-clock anchor. It advances when the transport subject changes and when a final seek commits a new anchor.
- `PlaybackFinalSeekRevision` identifies the most recently committed final seek. It advances only for final seeks, so consumers such as MPRIS can distinguish a seek discontinuity from a track transition or a newly sampled clock value.

Neither identity advances for seek previews or for ordinary elapsed-time progress.

### `PlaybackSourceState`

Enum (`std::uint8_t`): `Inactive`, `Live`, `Invalidated`.
Public mirror of the internal succession source state.

### `PlaybackSnapshot`

| Field | Type | Meaning |
|---|---|---|
| `revision` | `PlaybackRevision` | Identity of this published state. |
| `transport` | `PlaybackTransportSnapshot` | Transport, output, and quality portion. |
| `succession` | `PlaybackSuccessionSnapshot` | Live-source succession portion. |
| `preparation` | `PlaybackPreparationSnapshot` | Prepared-successor correlation portion. |

`sameContentAs(other)` compares semantic content except `revision` and the transport's correlated `elapsed` sample.
Full snapshot equality includes `revision`, but likewise excludes `elapsed` from semantic equality.

#### `PlaybackTransportSnapshot`

| Field | Type | Meaning |
|---|---|---|
| `transport` | `audio::Transport` | Idle, preparing, playing, or paused transport state. |
| `ready` | `bool` | Output readiness for the current subject. |
| `positionRevision` | `PlaybackPositionRevision` | Identity of the current playback-clock anchor. |
| `finalSeekRevision` | `PlaybackFinalSeekRevision` | Identity of the most recently committed final seek. |
| `elapsed` | `std::chrono::milliseconds` | Correlated position sample captured with this snapshot; UIModel interpolates from it. Clock progress alone is not semantic content and does not cause publication. |
| `duration` | `std::chrono::milliseconds` | Current subject duration. |
| `nowPlaying` | `NowPlayingInfo` | Current transport subject and its display metadata. |
| `volume` | `VolumeState` | Level, mute, availability. |
| `output` | `OutputState` | Selected device and available backends. |
| `quality` | `QualityState` | Accepted quality graph, carried with this revision. |

#### `PlaybackSuccessionSnapshot`

| Field | Type | Meaning |
|---|---|---|
| `sourceState` | `PlaybackSourceState` | Succession source lifecycle. |
| `currentTrackId` | `TrackId` | Succession subject; equals `transport.nowPlaying.trackId` while active. |
| `sourceListId` | `ListId` | Source list of the live projection. |
| `hasNext` | `bool` | A next successor is available. |
| `hasPrevious` | `bool` | A previous member is available. |
| `shuffle` | `ShuffleMode` | `Off` or `On`. |
| `repeat` | `RepeatMode` | `Off`, `One`, or `All`. |

#### `PlaybackPreparationSnapshot`

| Field | Type | Meaning |
|---|---|---|
| `hasPreparedNext` | `bool` | A successor is currently prepared. |

### `PlaybackEvents`

| Member | Signature | Meaning |
|---|---|---|
| `onSnapshot(handler)` | `async::Subscription` | Fires once per content-changing logical commit with the new snapshot. |
| `onPlaybackFailure(handler)` | `async::Subscription` | Delivers `PlaybackFailureEvent`. |
| `onSeekPreview(handler)` | `async::Subscription` | Delivers `PlaybackSeekPreview`. |
| `onRevealTrackRequested(handler)` | `async::Subscription` | Delivers the temporary `PlaybackRevealTrackRequest` navigation intent retained until RFC 0005 stage 6. |

`onSnapshot` is an event subscription: it announces publication of a new current `PlaybackSnapshot`.
Reading the current value remains a direct `PlaybackService::snapshot()` operation, so consumers do not need a separate snapshot-source object.
The borrowed reference returned by `snapshot()` must not be retained across a command or callback that can publish a newer snapshot; consumers that need an older value for comparison copy it explicitly.

#### `PlaybackFailureEvent`

| Field | Type | Meaning |
|---|---|---|
| `revision` | `PlaybackRevision` | Application revision current when the failure was observed. |
| `optCommandId` | `std::optional<PlaybackCommandId>` | Originating command when the failure remains correlated to an admitted service intent; absent for an uncorrelated lower asynchronous failure. |
| `failure` | `PlaybackFailure` | The typed failure payload. |

#### `PlaybackSeekPreview`

| Field | Type | Meaning |
|---|---|---|
| `revision` | `PlaybackRevision` | Revision this preview refines; a preview never advances the revision. |
| `elapsed` | `std::chrono::milliseconds` | Previewed position. |

`PlaybackCommandId` wraps a `std::uint64_t value`.

#### `PlaybackRevealTrackRequest`

| Field | Type | Meaning |
|---|---|---|
| `trackId` | `TrackId` | Track to reveal. |
| `preferredViewId` | `ViewId` | Preferred view, or the invalid id when unspecified. |
| `preferredListId` | `ListId` | Preferred list, or the invalid id when unspecified. |

### `PlaybackCommands`

| Command | Signature | Result |
|---|---|---|
| `startFromView` | `(ViewId, TrackId)` | `Result<>` admission result |
| `next` / `previous` / `clearSequence` | `()` | `void` |
| `setShuffleMode` / `setRepeatMode` | `(mode)` | `void` |
| `pause` / `resume` / `stop` | `()` | `void` |
| `seek` | `(std::chrono::milliseconds, PlaybackSeekMode = Final)` | `void` |
| `setOutputDevice` | `(BackendId, DeviceId, ProfileId)` | `void` |
| `setVolume` / `setMuted` | `(value)` | `void` |
| `revealPlayingTrack` | `()` | `void` |
| `revealTrack` | `(TrackId, ViewId = invalid, ListId = invalid)` | `void` |

`PlaybackSeekMode` is `Final` or `Preview`.
Session save, restore, and discard keep their call-level `Result` on `AppRuntime`; reveal is temporary and moves to an explicit navigation intent in RFC 0005 stage 6.
Every accepted command receives an internal intent generation and a public command id.
When a command is issued from a playback event handler, admission returns on that handler stack but execution occurs in a later executor turn.

## Validation rules

- Every method is callback-executor-affine; handlers run on the executor thread and must defer owner teardown to a later turn.
- While succession is active, `succession.currentTrackId` equals `transport.nowPlaying.trackId`. Idle transport may retain that coherent subject as a deferred restored session; an idle snapshot with no transport subject has no active succession subject.
- The public surface has no track/list-only start, stage, commit, or prepared-next mutation; every public start captures succession context through `startFromView`.
- `revision` identifies boundary-committed state, advances only when semantic content changes, and is strictly monotonic. A pending spontaneous lower-layer change is not visible through `snapshot()` until its deferred publication commits.
- Revision state advances for semantic commits even when no snapshot observer is connected.
- `elapsed` may be re-sampled while composing a semantic transition, but elapsed drift by itself does not advance `revision`, `positionRevision`, or `finalSeekRevision`.
- Final seeks advance both position identities. Subject changes advance only `positionRevision`; seek previews advance neither.
- One logical commit publishes at most one snapshot and advances at most one revision; a no-op publishes none. Spontaneous lower-layer changes already accepted in one executor turn coalesce into one external-settlement commit.
- A command issued from any `PlaybackEvents` handler executes in a later service turn and cannot alter the event or snapshot currently being delivered.
- A newer start, next, previous, restore, stop, clear, output-route change, or shutdown invalidates an older queued start/navigation intent; orthogonal quick commands remain ordered rather than being discarded.

## Compatibility and versioning

This is an in-process C++ API, not a persisted or wire format; it carries no schema version and the repository keeps no source-compatibility constraint for it.
Observable behavior and its invariants are owned by the playback specifications; this document owns only the exact names and fields.

## Implementation authority

- [`PlaybackService.h`](../../../app/include/ao/rt/playback/PlaybackService.h), [`PlaybackCommands.h`](../../../app/include/ao/rt/playback/PlaybackCommands.h), [`PlaybackEvents.h`](../../../app/include/ao/rt/playback/PlaybackEvents.h), and [`PlaybackSnapshot.h`](../../../app/include/ao/rt/playback/PlaybackSnapshot.h) declare the surface.
- [`PlaybackService.cpp`](../../../app/runtime/playback/PlaybackService.cpp) implements intent admission, supersession, coherent commits, revisioned publication, and composition bootstrap.

## Test authority

- [`PlaybackServiceTest.cpp`](../../../test/unit/runtime/PlaybackServiceTest.cpp) locks public-surface closure, snapshot coherence, revision and position-identity monotonicity, no-subscriber revision advancement, correlated output/readiness/quality, elapsed-insensitive semantic equality, observer deferral, supersession, failure correlation, pending-intent lifetime, and end-of-turn coalescing.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Playback application commits](../../spec/playback/application-commit.md)
- [RFC 0005: Coherent playback application boundary](../../rfc/0005-coherent-playback-boundary.md)
- [Playback session state](session-state.md)
- [Audio quality surface](quality-surface.md)
