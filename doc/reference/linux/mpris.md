---
id: linux-gtk.mpris-surface
---
# Linux MPRIS reference

## Scope and authority

This reference enumerates the MPRIS surface exported by the shared Linux system-media adapter for GTK and TUI.
Behavioral ownership, threading, failure, and lifetime belong to the [Linux MPRIS specification](../../system/frontend/mpris.md).
The introspection XML in [`MprisBridge.cpp`](../../../app/platform/media/linux/MprisBridge.cpp) and this protocol reference are the wire authorities.

## Endpoint identity

Every session exports the same object path and interfaces:

| Item | Value |
| --- | --- |
| Object path | `/org/mpris/MediaPlayer2` |
| Root interface | `org.mpris.MediaPlayer2` |
| Player interface | `org.mpris.MediaPlayer2.Player` |

Frontend-specific identity is:

| Frontend | Bus name | Identity | Desktop entry |
| --- | --- | --- | --- |
| GTK | `org.mpris.MediaPlayer2.aobus` | `Aobus` | `aobus` |
| TUI | `org.mpris.MediaPlayer2.aobus.tui.instance<suffix>` | `Aobus TUI` | not exported |

GTK requests its canonical name without queueing, so at most one GTK instance exports it.
Each TUI connection derives `<suffix>` from the unique name assigned by the bus, removes the leading colon, and replaces dots with underscores; for example, `:1.42` produces `org.mpris.MediaPlayer2.aobus.tui.instance1_42`.

## Root interface

| Member | Kind | GTK mapping/value | TUI mapping/value |
| --- | --- | --- | --- |
| `Raise` | method | present the injected GTK window | unsupported |
| `Quit` | method | request injected application quit after reply submission | request normal TUI exit after reply submission |
| `CanRaise` | property | `true` when the callback is installed | `false` |
| `CanQuit` | property | `true` when the callback is installed | `true` |
| `Fullscreen` | property | `false` | `false` |
| `CanSetFullscreen` | property | `false` | `false` |
| `HasTrackList` | property | `false` | `false` |
| `Identity` | property | `Aobus` | `Aobus TUI` |
| `DesktopEntry` | optional property | `aobus` | not exported |
| `SupportedUriSchemes` | property | empty array | empty array |
| `SupportedMimeTypes` | property | empty array | empty array |

The `Raise` member remains present in the common introspection surface when `CanRaise` is false; calling it then returns the adapter's unsupported-method error.
`OpenUri` is not exported.
Aobus installs no TUI desktop file, so the TUI omits the optional `DesktopEntry` property from introspection and `GetAll`; direct `Get` returns a property error.

## Player methods

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

A successful method reply acknowledges accepted submission, not eventual audio completion.
Root `Quit` submits its reply before the deferred host callback runs; shutdown still provides no unconditional peer-receipt guarantee.

## Player properties

| Property | Access | Mapping/value |
| --- | --- | --- |
| `PlaybackStatus` | read | transport mapping below |
| `LoopStatus` | read/write | repeat mapping below |
| `Rate` | read/write | reads `1.0`; zero pauses; other finite values accepted without change |
| `Shuffle` | read/write | sequence shuffle off/on |
| `Metadata` | read | metadata map below |
| `Volume` | read/write | `PlaybackState::volume.level` / `PlaybackCommands::setVolume` |
| `Position` | read | occurrence-correlated live `PlaybackService::elapsed()` converted to microseconds |
| `MinimumRate` | read | `1.0` |
| `MaximumRate` | read | `1.0` |
| `CanGoNext` | read | `isCapable(Next)` |
| `CanGoPrevious` | read | `isCapable(Previous)` |
| `CanPlay` | read | `isCapable(Play)` |
| `CanPause` | read | `isCapable(Pause)` |
| `CanSeek` | read | current track and occurrence are valid, with a known positive duration |
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

## Metadata

An invalid current track or zero occurrence produces an empty metadata map.
Present non-empty values use:

| Key | Value |
| --- | --- |
| `mpris:trackid` | `/org/mpris/MediaPlayer2/Track/<TrackId>_<PlaybackOccurrenceId>` |
| `xesam:title` | current title string |
| `xesam:artist` | one-element artist string array |
| `xesam:album` | current album string |
| `mpris:length` | positive duration in microseconds |
| `mpris:artUrl` | current non-empty local file URL; omitted while asynchronous export is pending or unavailable |

MPRIS artwork files use `<cache>/aobus/mpris-art-v2/<full-sha256><detected-extension>`.
The detected extensions are `.png`, `.jpg`, `.gif`, `.webp`, and fallback `.img`.
A memoized path is validated as a regular file with the recorded byte size; it is not read back for another digest check.

## Signals

The adapter emits `org.freedesktop.DBus.Properties.PropertiesChanged` for affected player fields, including `Metadata` when playback occurrence changes with otherwise repeated track metadata.
It emits `org.mpris.MediaPlayer2.Player.Seeked(position)` in microseconds for non-preview runtime seek updates.
For a new cover resource, the first `Metadata` change may omit `mpris:artUrl`; successful completion for the still-current resource emits a later `Metadata` change containing it.

## Validation rules

- Track object paths are empty for an invalid track or zero occurrence and stable for one valid `(TrackId, PlaybackOccurrenceId)` pair.
- Microsecond conversion saturates at signed 64-bit bounds and truncates to milliseconds on input.
- Relative seek captures the current occurrence and offset but samples position and decides past-end Next at execution. A positive offset strictly past the known end advances only with a successor and matching runtime/audio occurrence; otherwise it is a successful no-op.
- Other relative seek clamps at zero and known duration without overflow; exact end remains a final seek.
- Absolute `SetPosition` ignores stale occurrence-qualified track paths, negative positions, and positions beyond a known duration.
- Busy positioning joins the runtime FIFO. A stale occurrence at execution is a successful no-op with no final seek signal.
- `Position` uses the committed clock anchor when the live audio item no longer matches the published occurrence, rather than reporting a successor position under old metadata.
- Non-finite `Rate` writes are invalid.
- Unsupported loop strings, writable property names, and methods are rejected.

## Availability

The surface exists only when the Linux binary is built with `AOBUS_BUILD_SYSTEM_MEDIA`.
TUI `--system-media=auto` starts it best effort; `--system-media=off` constructs no adapter.
The same runtime option is accepted but inert in current Windows and macOS TUI builds.
An absent or unusable session bus leaves the optional surface unavailable, with no autolaunch or automatic reconnect.
An explicit bridge address overrides discovery; otherwise a present `DBUS_SESSION_BUS_ADDRESS` is used unchanged.
Only an unset bus variable permits discovery of an existing same-user `bus` socket under an absolute `XDG_RUNTIME_DIR`; a present empty value does not request fallback.
The adapter accepts validated `unix:path` and `unix:abstract` session-bus addresses using Unix `EXTERNAL` authentication; it rejects non-Unix or mixed-transport fallback lists.
See [build development](../../development/build.md) and the [TUI command reference](../tui/command.md) for feature and runtime selection.

## Compatibility

The exported surface is unversioned.
Changing a bus-name rule, member name, type, access mode, or mapping requires updating the source introspection XML, this reference, and focused protocol tests together.

Track id `42` at playback occurrence `7` maps to `/org/mpris/MediaPlayer2/Track/42_7`.
A duration of 125 seconds maps to `125000000` microseconds.

## Implementation authority

- [`MprisBridge.cpp`](../../../app/platform/media/linux/MprisBridge.cpp) contains introspection, D-Bus encoding, and property/signal mapping.
- [`MprisBusSession.cpp`](../../../app/platform/media/linux/MprisBusSession.cpp) owns connection and bus-name mechanics.
- [`MprisPlaybackEndpoint.h`](../../../app/platform/media/linux/MprisPlaybackEndpoint.h) contains method, property-write, and capability mapping.
- [`MprisArtUrlSession.h`](../../../app/platform/media/linux/MprisArtUrlSession.h) owns current-resource correlation.
- [`MprisArtUrlCache.cpp`](../../../app/platform/media/linux/MprisArtUrlCache.cpp) owns file naming and URL export.

## Test authority

Focused sources are [`MprisBridgeTest.cpp`](../../../test/integration/linux/media/MprisBridgeTest.cpp), [`MprisPlaybackPositionTest.cpp`](../../../test/integration/linux/media/MprisPlaybackPositionTest.cpp), [`MprisArtUrlSessionTest.cpp`](../../../test/integration/linux/media/MprisArtUrlSessionTest.cpp), [`MprisBridgeIntegrationTest.cpp`](../../../test/integration/linux/media/MprisBridgeIntegrationTest.cpp), [`MprisBusSessionTest.cpp`](../../../test/integration/linux/media/MprisBusSessionTest.cpp), and [`MprisSignalTest.cpp`](../../../test/integration/linux/media/MprisSignalTest.cpp).

## Related documents

- [Linux MPRIS specification](../../system/frontend/mpris.md)
- [Playback architecture](../../system/playback/README.md)
- [Cover-art resource delivery](../../system/resource/cover-art-delivery.md)
