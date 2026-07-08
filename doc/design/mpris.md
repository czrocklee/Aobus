# MPRIS Integration

Aobus exposes the Linux desktop media-player surface through MPRIS on the GTK
frontend. The runtime and core library stay D-Bus-free; the bridge lives in the
Linux GTK layer and translates between D-Bus calls/properties and existing
application-level playback commands.

## Boundary

`app/linux-gtk/platform/MprisBridge` owns the D-Bus integration:

- bus name: `org.mpris.MediaPlayer2.aobus`;
- object path: `/org/mpris/MediaPlayer2`;
- interfaces: `org.mpris.MediaPlayer2` and `org.mpris.MediaPlayer2.Player`;
- state source: `rt::PlaybackService`;
- command sink: `uimodel::PlaybackCommandSurface`.

The bridge must never call layout components or transport button instances
directly. Layout components are optional, user-customizable, and rebuilt during
layout edits. MPRIS commands therefore execute `PlaybackCommand` values through
`PlaybackCommandSurface`, the same owner used by shell actions, shortcuts, and
transport buttons.

## Bus Name Policy

Aobus uses a single canonical MPRIS name. The first GTK instance that acquires
`org.mpris.MediaPlayer2.aobus` exports MPRIS. A second instance, or any other
name-acquisition failure, logs a warning and disables MPRIS for that instance.
It must not crash, post noisy user-facing errors, or disturb the first
instance.

This policy keeps `playerctl -p aobus ...` predictable and avoids inventing a
second-instance product policy inside MPRIS. A future single-instance
application policy can reuse the same behavior.

## Implemented Surface

The current bridge covers transport control, playback status, now-playing
metadata, seek dispatch, live repeat/shuffle state, volume, and cover art URLs.

### Commands

| MPRIS method/property | Aobus sink |
|---|---|
| `PlayPause` | `PlaybackCommand::PlayPause` |
| `Play` | `PlaybackCommand::Play` |
| `Pause` | `PlaybackCommand::Pause` |
| `Stop` | `PlaybackCommand::Stop` |
| `Next` | `PlaybackCommand::Next` |
| `Previous` | `PlaybackCommand::Previous` |
| `Seek` | relative seek through `PlaybackService::seek()` |
| `SetPosition` | absolute seek through `PlaybackService::seek()` when the MPRIS track id matches the current track |
| `Rate` property write | finite non-zero values are accepted and ignored; `0.0` pauses because Aobus is fixed-rate |
| `Volume` property write | `PlaybackService::setVolume()` |
| `Shuffle` property write | `PlaybackService::setShuffleMode()` |
| `LoopStatus` property write | `PlaybackService::setRepeatMode()` |
| `Raise` | present the GTK main window |
| `Quit` | request GTK application quit |

`Raise` and `Quit` stay inside the GTK composition root. The bridge receives
callbacks from `MainWindow` and never reaches into runtime/core for application
lifecycle. Player methods that map to a known command are accepted; the command
surface decides whether execution has an effect. Unknown methods return a D-Bus
error.

### Properties

`PlaybackStatus` maps from `PlaybackService::state().transport`:

| Transport | MPRIS |
|---|---|
| `Playing`, `Opening`, `Buffering`, `Seeking` | `Playing` |
| `Paused` | `Paused` |
| everything else | `Stopped` |

The player reports these capabilities:

- `CanControl=true`; the MPRIS spec does not expect it to change while the
  player is running;
- `CanRaise=true`, `CanQuit=true` when the GTK callbacks are installed;
- `CanPlay`, `CanPause`, `CanGoNext`, and `CanGoPrevious` come from
  `PlaybackCommandSurface::capable()`, not GTK action enablement;
- `CanSeek=true` only when a current track exists;
- `Metadata` includes `mpris:trackid`, `xesam:title`, `xesam:artist`,
  `xesam:album`, `mpris:length`, and `mpris:artUrl` when those fields are
  available for the current track;
- `Position` is the current playback position in microseconds;
- `Rate`, `MinimumRate`, and `MaximumRate` all read as `1.0`; finite non-zero
  rate writes are accepted and ignored because Aobus does not support
  variable-rate playback, while `0.0` acts like `Pause`;
- `Volume` is the current playback volume and is writable; writes use the same
  normalization and backend path as the in-app volume controls;
- `Shuffle` reflects `PlaybackState::shuffleMode` and is writable;
- `LoopStatus` maps repeat off/one/all to `None`/`Track`/`Playlist` and is
  writable;
- `SupportedUriSchemes=[]` because the bridge does not implement `OpenUri`.

`mpris:trackid` is stable for the current `TrackId` and uses
`/org/mpris/MediaPlayer2/Track/<TrackId>`. `SetPosition` no-ops when the
caller passes any other object path, matching the MPRIS stale-track guard.
Absolute `SetPosition` requests outside the current track, and seek requests
when no current track exists, are no-ops. Relative `Seek` requests before zero
clamp to zero; requests beyond the known duration delegate to `Next` and are
otherwise accepted as no-ops when no next item is available.

Capability ownership:

| MPRIS property | Query |
|---|---|
| `CanPlay` | `PlaybackCommandSurface::capable(PlaybackCommand::Play)` |
| `CanPause` | `PlaybackCommandSurface::capable(PlaybackCommand::Pause)` |
| `CanGoNext` | `PlaybackCommandSurface::capable(PlaybackCommand::Next)` |
| `CanGoPrevious` | `PlaybackCommandSurface::capable(PlaybackCommand::Previous)` |
| `CanControl` | constant `true` |
| `CanSeek` | `PlaybackService::state().nowPlaying.trackId != kInvalidTrackId` |

## Signals

The bridge emits `org.freedesktop.DBus.Properties.PropertiesChanged` for
`org.mpris.MediaPlayer2.Player` whenever playback transport changes affect
`PlaybackStatus`, whenever `PlaybackCommandSurface::onAvailabilityChanged()`
reports capability changes, whenever now-playing metadata changes, and whenever
volume, repeat, or shuffle state changes. Explicit final seeks emit the MPRIS
`Seeked` signal with the new position in microseconds. In-app preview seeks are
local drag feedback and do not emit `Seeked`.

## Threading

The GTK bridge runs on the GTK main context. `PlaybackService` and
`PlaybackCommandSurface` callbacks are subscribed in the same process and
scheduled by the runtime executor; the bridge only reads `PlaybackService`
state, executes playback commands, and calls injected GTK lifecycle callbacks.
No D-Bus callback may touch audio engine internals or block on decoder work.

## Ownership Rules

1. GTK layout actions, transport buttons, and MPRIS adapt playback commands;
   they do not define command semantics.
2. External protocols talk to `PlaybackCommandSurface`, never to the layout
   action registry.
3. Queue consistency under playback-mode changes is guaranteed by
   `PlaybackQueueSession` subscriptions to `PlaybackService` mode events.
4. Command availability has one owner and one change event:
   `PlaybackCommandSurface::onAvailabilityChanged()`.
5. View models describe presentation. Command execution and enablement live in
   the command surface.

## Art And Metadata

The implemented metadata surface fills `Metadata` with title, artist, album,
duration, stable track object path, and current cover art when present. Runtime
state exposes only the primary cover-art `ResourceId`; GTK resolves that
resource through `MprisArtUrlCache`, exporting the original resource bytes to
the user cache directory and returning a `file://` URL. Valid exported files are
memoized for the process; missing or size-mismatched files are rewritten, and
stale sibling files from older extension guesses are removed. The bridge only
sees an injected resolver callback, so runtime/core remain D-Bus-free and
file-URL-free. Tracks without cover art omit `mpris:artUrl`.

## Degradation

D-Bus connection, object-registration, or name-ownership failure disables the
bridge for that instance and logs the reason. Playback and the rest of the GTK
application continue normally.
