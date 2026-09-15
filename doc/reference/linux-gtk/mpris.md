---
id: linux-gtk.mpris-surface
type: reference
status: current
domain: presentation
summary: Enumerates the GTK MPRIS bus identity, interfaces, methods, properties, metadata keys, mappings, and signals.
---
# GTK MPRIS reference

## Scope and version

This reference enumerates the currently exported MPRIS surface.
Behavioral ownership, failure, and lifetime belong to the [GTK MPRIS specification](../../spec/linux-gtk/mpris.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places the protocol adapter in the GTK frontend.
The exact introspection authority is `app/linux-gtk/platform/MprisBridge.cpp`.
Command mapping authority is `MprisPlaybackEndpoint.h`.
No runtime or core header exports these protocol names.

## Surface

| Item | Value |
| --- | --- |
| Bus name | `org.mpris.MediaPlayer2.aobus` |
| Object path | `/org/mpris/MediaPlayer2` |
| Root interface | `org.mpris.MediaPlayer2` |
| Player interface | `org.mpris.MediaPlayer2.Player` |
| Desktop entry | `aobus` |
| Identity | `Aobus` |

### Root interface

| Member | Kind | Mapping/value |
| --- | --- | --- |
| `Raise` | method | injected GTK present callback |
| `Quit` | method | injected GTK quit callback |
| `CanRaise` | property | whether raise callback exists |
| `CanQuit` | property | whether quit callback exists |
| `Fullscreen` | property | `false` |
| `CanSetFullscreen` | property | `false` |
| `HasTrackList` | property | `false` |
| `Identity` | property | `Aobus` |
| `DesktopEntry` | property | `aobus` |
| `SupportedUriSchemes` | property | empty array |
| `SupportedMimeTypes` | property | empty array |

`OpenUri` is not exported.

### Player methods

| Method | Aobus mapping |
| --- | --- |
| `PlayPause` | `PlaybackCommand::PlayPause` |
| `Play` | `PlaybackCommand::Play` |
| `Pause` | `PlaybackCommand::Pause` |
| `Stop` | `PlaybackCommand::Stop` |
| `Next` | `PlaybackCommand::Next` through the ordinary capability-gated command path |
| `Previous` | `PlaybackCommand::Previous` |
| `Seek(offset)` | queued occurrence-guarded relative positioning; execution-time live elapsed determines final seek or guarded past-end Next |
| `SetPosition(track, position)` | queued final seek for the current occurrence-qualified track path and valid range, revalidated at execution |

### Player properties

| Property | Access | Mapping/value |
| --- | --- | --- |
| `PlaybackStatus` | read | transport mapping below |
| `LoopStatus` | read/write | repeat mapping below |
| `Rate` | read/write | reads `1.0`; zero pauses; other finite values accepted without change |
| `Shuffle` | read/write | sequence shuffle off/on |
| `Metadata` | read | metadata map below |
| `Volume` | read/write | `PlaybackState::volume.level` / `PlaybackService::setVolume` |
| `Position` | read | occurrence-correlated live `PlaybackService::elapsed()` converted to microseconds |
| `MinimumRate` | read | `1.0` |
| `MaximumRate` | read | `1.0` |
| `CanGoNext` | read | `isCapable(Next)` |
| `CanGoPrevious` | read | `isCapable(Previous)` |
| `CanPlay` | read | `isCapable(Play)` |
| `CanPause` | read | `isCapable(Pause)` |
| `CanSeek` | read | current track id and occurrence are valid, with a known positive duration |
| `CanControl` | read | `true` |

Transport mapping:

| Aobus transport | `PlaybackStatus` |
| --- | --- |
| `Opening`, `Buffering`, `Seeking`, `Playing` | `Playing` |
| `Paused` | `Paused` |
| `Idle`, `Stopping`, `Error` | `Stopped` |

Repeat mapping:

| Aobus repeat | `LoopStatus` |
| --- | --- |
| `Off` | `None` |
| `One` | `Track` |
| `All` | `Playlist` |

### Metadata

An invalid current track produces an empty metadata map.
Present non-empty values use:

| Key | Value |
| --- | --- |
| `mpris:trackid` | `/org/mpris/MediaPlayer2/Track/<TrackId>_<PlaybackOccurrenceId>` |
| `xesam:title` | current title string |
| `xesam:artist` | one-element artist string array |
| `xesam:album` | current album string |
| `mpris:length` | positive duration in microseconds |
| `mpris:artUrl` | resolved non-empty local file URL; omitted while asynchronous cache-file export is pending |

### Signals

The bridge emits `org.freedesktop.DBus.Properties.PropertiesChanged` for affected player fields, including `Metadata` when playback occurrence changes with otherwise repeated track metadata.
It emits `org.mpris.MediaPlayer2.Player.Seeked(position)` in microseconds for non-preview runtime seek updates.
For a new cover resource, the first `Metadata` change may omit `mpris:artUrl`; successful current-resource file export emits a later `Metadata` change containing it.

## Validation rules

- Track object paths are empty for an invalid track or zero occurrence and stable for one valid `(TrackId, PlaybackOccurrenceId)` pair.
- Microsecond conversion saturates at signed 64-bit bounds and truncates to milliseconds on input.
- Relative seek captures the current occurrence and offset, but samples position and decides past-end Next at execution. A positive offset strictly past the known end advances only with a successor and matching runtime/audio occurrence; otherwise it is a successful no-op. Standalone `Next` retains the ordinary capability-gated command path.
- Other relative seek clamps at zero and known duration without overflow; exact end remains a final seek.
- Absolute `SetPosition` ignores stale occurrence-qualified track paths, negative positions, and positions beyond a known duration.
- Busy positioning joins the runtime FIFO. A stale occurrence at execution is a successful no-op, with no final seek signal; success is not an audio-completion guarantee.
- `Position` uses the committed clock anchor when the live audio item no longer matches the published occurrence, rather than reporting a successor's position under old metadata.
- Non-finite `Rate` writes are invalid.
- Unsupported loop strings and writable property names are rejected.

## Compatibility and versioning

The exported surface is unversioned in-process GTK behavior governed by the MPRIS interface names above.
Changing a name, type, access mode, or mapping requires updating the introspection XML, this reference, and protocol tests together.

## Examples

Track id `42` at playback occurrence `7` maps to `/org/mpris/MediaPlayer2/Track/42_7`.
A duration of 125 seconds maps to `125000000` microseconds.

## Implementation authority

- [`MprisBridge.cpp`](../../../app/linux-gtk/platform/MprisBridge.cpp) contains the introspection XML and D-Bus encoding.
- [`MprisPlaybackEndpoint.h`](../../../app/linux-gtk/platform/MprisPlaybackEndpoint.h) contains method, property-write, and capability mapping.

## Test authority

- [`MprisBridgeTest.cpp`](../../../test/unit/linux-gtk/platform/MprisBridgeTest.cpp) protects the listed mappings and constraints.

## Related documents

- [GTK MPRIS specification](../../spec/linux-gtk/mpris.md)
- [Playback architecture](../../architecture/playback.md)
- [Cover-art resource delivery specification](../../spec/resource/cover-art-delivery.md)
