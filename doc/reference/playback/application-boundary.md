---
id: playback.application-boundary
type: reference
status: current
domain: playback
summary: Enumerates the coherent PlaybackService surface — its current snapshot, event subscriptions, and commands exposed by AppRuntime.
---
# Playback application boundary

## Scope and version

This reference owns the exact public types of the coherent playback boundary introduced by [RFC 0005](../../rfc/0005-coherent-playback-boundary.md): `PlaybackService`, its `PlaybackCommands` and `PlaybackEvents` roles, and the immutable `PlaybackSnapshot` plus revisioned event payloads.

It is the stage-2 surface.
`PlaybackService` currently adapts the runtime-internal `PlaybackTransport` and `PlaybackSuccession` owners, while `AppRuntime` exposes only the public service to consumers.
Those internal headers live under `app/runtime/playback/` and repository guardrails reject their use by public runtime headers, UIModel, and frontends.
Later RFC 0005 stages move commit authority into a coordinator.
The exact fields below are current; their revision-commit semantics tighten when the coordinator lands, as noted in [validation rules](#validation-rules).

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
| `snapshot()` | `PlaybackSnapshot` | Returns the current coherent state on demand. |
| `commands()` | `PlaybackCommands&` | The mutation port. |
| `events()` | `PlaybackEvents&` | The publication and transient-event subscription port. |

### `PlaybackRevision`

| Field | Type | Default | Meaning |
|---|---|---|---|
| `value` | `std::uint64_t` | `0` | Monotonic identity of one accepted application transition. Totally ordered; never derived from an Engine item id, audio generation, or persistence revision. |

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

`sameContentAs(other)` compares every portion except `revision`.

#### `PlaybackTransportSnapshot`

| Field | Type | Meaning |
|---|---|---|
| `transport` | `audio::Transport` | Idle, preparing, playing, or paused transport state. |
| `ready` | `bool` | Output readiness for the current subject. |
| `elapsed` | `std::chrono::milliseconds` | Committed position at snapshot time; UIModel interpolates between snapshots. |
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
| `onSnapshot(handler)` | `async::Subscription` | Fires once per accepted transition with the new snapshot. |
| `onPlaybackFailure(handler)` | `async::Subscription` | Delivers `PlaybackFailureEvent`. |
| `onSeekPreview(handler)` | `async::Subscription` | Delivers `PlaybackSeekPreview`. |
| `onRevealTrackRequested(handler)` | `async::Subscription` | Delivers the temporary `PlaybackRevealTrackRequest` navigation intent retained until RFC 0005 stage 6. |

`onSnapshot` is an event subscription: it announces publication of a new current `PlaybackSnapshot`.
Reading the current value remains a direct `PlaybackService::snapshot()` operation, so consumers do not need a separate snapshot-source object.

#### `PlaybackFailureEvent`

| Field | Type | Meaning |
|---|---|---|
| `revision` | `PlaybackRevision` | Application revision current when the failure was observed. |
| `optCommandId` | `std::optional<PlaybackCommandId>` | Originating command once preparation is asynchronous; absent in the stage-2 adapter. |
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

## Validation rules

- Every method is callback-executor-affine; handlers run on the executor thread and must defer owner teardown to a later turn.
- While active, `succession.currentTrackId` equals `transport.nowPlaying.trackId`; an idle transport has no active succession subject.
- The public surface has no track/list-only start, stage, commit, or prepared-next mutation; every public start captures succession context through `startFromView`.
- `revision` advances only when published content changes and is strictly monotonic.
- Stage-2 publication guarantee: one command issued through `PlaybackCommands` publishes at most one snapshot, and spontaneous lower-layer changes coalesce into one publication per executor turn. The stronger "one logical commit, one revision" guarantee is delivered when the coordinator replaces this adapter.

## Compatibility and versioning

This is an in-process C++ API, not a persisted or wire format; it carries no schema version and the repository keeps no source-compatibility constraint for it.
Observable behavior and its invariants are owned by the playback specifications; this document owns only the exact names and fields.

## Implementation authority

- [`PlaybackService.h`](../../../app/include/ao/rt/playback/PlaybackService.h), [`PlaybackCommands.h`](../../../app/include/ao/rt/playback/PlaybackCommands.h), [`PlaybackEvents.h`](../../../app/include/ao/rt/playback/PlaybackEvents.h), and [`PlaybackSnapshot.h`](../../../app/include/ao/rt/playback/PlaybackSnapshot.h) declare the surface.
- [`PlaybackService.cpp`](../../../app/runtime/playback/PlaybackService.cpp) implements the stage-2 adapter and composition bootstrap.

## Test authority

- [`PlaybackServiceTest.cpp`](../../../test/unit/runtime/PlaybackServiceTest.cpp) locks public-surface closure, snapshot coherence, revision monotonicity, correlated output/readiness/quality, single-publication-per-command, and end-of-turn coalescing.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [RFC 0005: Coherent playback application boundary](../../rfc/0005-coherent-playback-boundary.md)
- [Playback session state](session-state.md)
- [Audio quality surface](quality-surface.md)
