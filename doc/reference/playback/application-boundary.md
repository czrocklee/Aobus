---
id: playback.application-boundary
type: reference
status: current
domain: playback
summary: Enumerates the coherent PlaybackService snapshot, events, and commands exposed by AppRuntime.
---
# Playback application boundary

## Scope and version

This reference enumerates the current in-process `ao::rt` playback service:
`PlaybackService`, its `PlaybackCommands` and `PlaybackEvents` roles, and
`PlaybackSnapshot`. It has no serialized or wire-protocol version. Observable
behavior belongs to the
[playback application-commit specification](../../spec/playback/application-commit.md).

## Code boundary

This surface belongs to the **application runtime** layer in the
[system architecture](../../architecture/system-overview.md), under the
[playback architecture](../../architecture/playback.md). Public declarations
live in `app/include/ao/rt/playback/`, and `AppRuntime::playback()` exposes the
service. `app/runtime/playback/PlaybackService.cpp` implements the service
while borrowing the runtime-internal `PlaybackTransport` and
`PlaybackSuccession`; UIModel and frontends may depend on the public surface,
not those internal collaborators.

## Surface

### `PlaybackService`

| Member | Type | Meaning |
|---|---|---|
| `snapshot()` | `PlaybackSnapshot const&` | Borrows the last coherent state; stable until the next publication or service destruction. |
| `commands()` | `PlaybackCommands&` | The mutation port. |
| `events()` | `PlaybackEvents&` | Snapshot and transient-event subscriptions. |

### Position identities

`PlaybackPositionRevision` and `PlaybackFinalSeekRevision` each wrap a
comparable `std::uint64_t value`, defaulting to `0`.

- `PlaybackPositionRevision` identifies the playback-clock anchor. It advances
  for subject changes, successful restores, and final seeks.
- `PlaybackFinalSeekRevision` identifies the most recent committed final seek.
  It advances only for final seeks, allowing clock consumers to distinguish a
  seek from a track transition.

Neither advances for seek previews or ordinary elapsed-time progress.

### `PlaybackSourceState`

Enum (`std::uint8_t`): `Inactive`, `Live`, `Invalidated`.

### `PlaybackSnapshot`

| Field | Type | Meaning |
|---|---|---|
| `transport` | `PlaybackTransportSnapshot` | Transport, current subject, output, volume, and quality. |
| `succession` | `PlaybackSuccessionSnapshot` | Live-source and next/previous policy. |

Full equality is semantic equality. The nested transport comparison excludes
its correlated `elapsed` sample, so clock drift alone does not publish another
snapshot.

#### `PlaybackTransportSnapshot`

| Field | Type | Meaning |
|---|---|---|
| `transport` | `audio::Transport` | Idle, preparing, playing, or paused state. |
| `ready` | `bool` | Output readiness for the current subject. |
| `positionRevision` | `PlaybackPositionRevision` | Current playback-clock anchor identity. |
| `finalSeekRevision` | `PlaybackFinalSeekRevision` | Most recent committed final-seek identity. |
| `elapsed` | `std::chrono::milliseconds` | Position sampled while composing this snapshot. |
| `duration` | `std::chrono::milliseconds` | Current subject duration. |
| `nowPlaying` | `NowPlayingInfo` | Current subject and display metadata. |
| `volume` | `VolumeState` | Level, mute, availability, and backend capability. |
| `output` | `OutputState` | Selected device and available backends. |
| `quality` | `QualityState` | Accepted quality graph. |

#### `PlaybackSuccessionSnapshot`

| Field | Type | Meaning |
|---|---|---|
| `sourceState` | `PlaybackSourceState` | Succession source lifecycle. |
| `currentTrackId` | `TrackId` | Current succession subject. |
| `sourceListId` | `ListId` | Source list of the live projection. |
| `hasNext` / `hasPrevious` | `bool` | Whether the corresponding navigation is available. |
| `shuffle` | `ShuffleMode` | `Off` or `On`. |
| `repeat` | `RepeatMode` | `Off`, `One`, or `All`. |

### `PlaybackEvents`

| Member | Signature | Meaning |
|---|---|---|
| `onSnapshot(handler)` | `async::Subscription` | Fires once when a logical commit changes snapshot content. |
| `onSeekPreview(handler)` | `async::Subscription` | Delivers the previewed `std::chrono::milliseconds`. |
| `onRevealTrackRequested(handler)` | `async::Subscription` | Delivers `PlaybackRevealTrackRequest`. |

`snapshot()` remains the direct current-value operation. A borrowed snapshot
reference must not be retained across a command or callback that may publish a
new value; copy it when an older value is needed for comparison.

#### `PlaybackRevealTrackRequest`

| Field | Type | Meaning |
|---|---|---|
| `trackId` | `TrackId` | Track to reveal. |
| `preferredViewId` | `ViewId` | Preferred view, or the invalid id. |
| `preferredListId` | `ListId` | Preferred list, or the invalid id. |

### `PlaybackCommands`

| Command | Signature | Result |
|---|---|---|
| `startFromView` | `(ViewId, TrackId)` | `Result<>`; synchronous validation and async-task admission, or queued command admission. Success does not prove that the decoder opened. |
| `next` / `previous` / `clearSequence` | `()` | `void` |
| `setShuffleMode` / `setRepeatMode` | `(mode)` | `void` |
| `pause` / `resume` / `stop` | `()` | `void` |
| `seek` | `(milliseconds, PlaybackSeekMode = Final)` | `void` |
| `setOutputDevice` | `(BackendId, DeviceId, ProfileId)` | `void` |
| `setVolume` / `setMuted` | `(value)` | `void` |
| `revealPlayingTrack` | `()` | `void` |
| `revealTrack` | `(TrackId, ViewId = invalid, ListId = invalid)` | `void` |

`PlaybackSeekMode` is `Final` or `Preview`. Session save, restore, and discard
retain their call-level `Result` on `AppRuntime`.

## Validation rules

- A borrowed snapshot reference is valid only until the next publication or
  service destruction.
- `PlaybackTransportSnapshot` equality excludes `elapsed`; the two position
  identities make discontinuities part of semantic equality.
- `PlaybackSeekMode` accepts only `Final` and `Preview`.
- Reveal requests default unspecified view and list fields to their invalid ids.

Command admission, ordering, supersession, snapshot coherence, publication,
failure, and shutdown behavior belong to the
[playback application-commit specification](../../spec/playback/application-commit.md).

## Compatibility and versioning

This is an in-process C++ API, not a persisted or wire format. The repository
keeps no source-compatibility constraint for it.

## Implementation authority

- [`PlaybackService.h`](../../../app/include/ao/rt/playback/PlaybackService.h),
  [`PlaybackCommands.h`](../../../app/include/ao/rt/playback/PlaybackCommands.h),
  [`PlaybackEvents.h`](../../../app/include/ao/rt/playback/PlaybackEvents.h), and
  [`PlaybackSnapshot.h`](../../../app/include/ao/rt/playback/PlaybackSnapshot.h)
  declare the surface.
- [`PlaybackService.cpp`](../../../app/runtime/playback/PlaybackService.cpp)
  implements command ordering, supersession, commit composition, and
  publication.

## Test authority

- [`PlaybackServiceTest.cpp`](../../../test/unit/runtime/PlaybackServiceTest.cpp)
  protects surface closure, coherent state, position identities,
  elapsed-insensitive equality, observer deferral, supersession, queue lifetime,
  and end-of-turn coalescing.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Playback application commits](../../spec/playback/application-commit.md)
- [Playback session state](session-state.md)
- [Audio quality surface](quality-surface.md)
