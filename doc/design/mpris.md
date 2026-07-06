# MPRIS Integration

Aobus exposes the Linux desktop media-player surface through MPRIS on the GTK
frontend. The runtime and core library stay D-Bus-free; the bridge lives in the
Linux GTK layer and translates between D-Bus calls/properties and existing
application-level playback actions.

## Boundary

`app/linux-gtk/platform/MprisBridge` owns the D-Bus integration:

- bus name: `org.mpris.MediaPlayer2.aobus`;
- object path: `/org/mpris/MediaPlayer2`;
- interfaces: `org.mpris.MediaPlayer2` and `org.mpris.MediaPlayer2.Player`;
- state source: `rt::PlaybackService`;
- command sink: the app-level action dispatcher already used by keyboard
  shortcuts and layout actions.

The bridge must never call layout components or transport button instances
directly. Layout components are optional, user-customizable, and rebuilt during
layout edits. MPRIS commands therefore activate stable action ids such as
`playback.playPause`, `playback.next`, `playback.previous`, and
`playback.stop` through `ShellLayoutController::activateAction()`.

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

| MPRIS method | Aobus action |
|---|---|
| `PlayPause` | `playback.playPause` |
| `Play` | `playback.play` |
| `Pause` | `playback.pause` |
| `Stop` | `playback.stop` |
| `Next` | `playback.next` |
| `Previous` | `playback.previous` |
| `Seek` | relative seek through `PlaybackService::seek()` |
| `SetPosition` | absolute seek through `PlaybackService::seek()` when the MPRIS track id matches the current track |
| `Volume` property write | `PlaybackService::setVolume()` |
| `Shuffle` property write | `PlaybackService::setShuffleMode()` |
| `LoopStatus` property write | `PlaybackService::setRepeatMode()` |
| `Raise` | present the GTK main window |
| `Quit` | request GTK application quit |

`Raise` and `Quit` stay inside the GTK composition root. The bridge receives
callbacks from `MainWindow` and never reaches into runtime/core for application
lifecycle. Player methods that map to a known but currently disabled action are
accepted as no-ops; unknown methods return a D-Bus error.

### Properties

`PlaybackStatus` maps from `PlaybackService::state().transport`:

| Transport | MPRIS |
|---|---|
| `Playing`, `Opening`, `Buffering` | `Playing` |
| `Paused` | `Paused` |
| everything else | `Stopped` |

The player reports these capabilities:

- `CanControl=true` when at least one player command or seek target is
  available;
- `CanRaise=true`, `CanQuit=true` when the GTK callbacks are installed;
- `CanPlay`, `CanPause`, `CanGoNext`, and `CanGoPrevious` reflect the same
  action availability used by shell controls and keyboard shortcuts;
- `CanSeek=true` only when a current track exists;
- `Metadata` includes `mpris:trackid`, `xesam:title`, `xesam:artist`,
  `xesam:album`, `mpris:length`, and `mpris:artUrl` when those fields are
  available for the current track;
- `Position` is the current playback position in microseconds;
- `Volume` is the current playback volume and is writable; writes use the same
  normalization and backend path as the in-app volume controls;
- `Shuffle` reflects `PlaybackState::shuffleMode` and is writable;
- `LoopStatus` maps repeat off/one/all to `None`/`Track`/`Playlist` and is
  writable;
- `SupportedUriSchemes=[]` because the bridge does not implement `OpenUri`.

`mpris:trackid` is stable for the current `TrackId` and uses
`/org/mpris/MediaPlayer2/Track/<TrackId>`. `SetPosition` no-ops when the
caller passes any other object path, matching the MPRIS stale-track guard.
Absolute `SetPosition` requests outside the current track are no-ops. Relative
`Seek` requests before zero clamp to zero; requests beyond the known duration
delegate to `Next` and are otherwise accepted as no-ops when no next item is
available.

## Signals

The bridge emits `org.freedesktop.DBus.Properties.PropertiesChanged` for
`org.mpris.MediaPlayer2.Player` whenever the playback transport changes enough
to affect `PlaybackStatus` or live capabilities, whenever now-playing metadata
changes, and whenever volume, repeat, or shuffle state changes. Explicit final
seeks emit the MPRIS `Seeked` signal with the new position in microseconds.
In-app preview seeks are local drag feedback and do not emit `Seeked`.

## Threading

The GTK bridge runs on the GTK main context. `PlaybackService` callbacks are
subscribed in the same process and scheduled by the runtime executor; the bridge
only reads `PlaybackService::state()` and activates GTK action dispatchers from
the UI layer. No D-Bus callback may touch audio engine internals or block on
decoder work.

## Art And Metadata

The implemented metadata surface fills `Metadata` with title, artist, album,
duration, stable track object path, and current cover art when present. Runtime
state exposes only the primary cover-art `ResourceId`; GTK resolves that
resource through `MprisArtUrlCache`, exporting the original resource bytes to
the user cache directory and returning a `file://` URL. The cache file is
rewritten from the current library resource whenever a URL is requested, so
deleted or truncated cache files and reused resource ids do not leave stale
URLs behind. The bridge only sees an injected resolver callback, so
runtime/core remain D-Bus-free and file-URL-free. Tracks without cover art omit
`mpris:artUrl`.

## Degradation

D-Bus connection, object-registration, or name-ownership failure disables the
bridge for that instance and logs the reason. Playback and the rest of the GTK
application continue normally.
