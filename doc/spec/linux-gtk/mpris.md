---
id: linux-gtk.mpris
type: spec
status: current
domain: presentation
summary: Defines GTK MPRIS ownership, command routing, observation, bus-name lifecycle, degradation, and teardown behavior.
---
# GTK MPRIS specification

## Scope

This specification owns the GTK frontend's MPRIS adapter behavior.
It defines protocol ownership, single-name policy, command and state authority, observation, signal timing, cover-art resolver boundary, degradation, and lifetime.
The exact D-Bus names, methods, properties, mappings, and metadata keys belong to the [MPRIS reference](../../reference/linux-gtk/mpris.md).

## Code boundary

`ao::gtk::platform::MprisBridge` and `MprisPlaybackEndpoint` live entirely in `app/linux-gtk/platform/`.
Core audio and runtime remain D-Bus-free.

The bridge reads the coherent `rt::PlaybackService` snapshot and executes transport and succession commands through `uimodel::PlaybackCommandSurface`.
It never calls transport widgets or layout components, because those surfaces are optional and rebuildable.
GTK application lifetime enters only through injected raise and quit callbacks; cover art enters only through an injected `ResourceId` to URL resolver.

## Terminology

- The **canonical name** is the one application-wide MPRIS bus name.
- The **endpoint** is the D-Bus-free command/property mapping object used by the bridge and unit tests.
- A **stale track path** is a `SetPosition` track object path that does not identify the current runtime track.
- A **final seek** is a runtime seek update whose mode is not preview.

## Invariants

- At most one Aobus GTK instance exports the canonical MPRIS name.
- Failure to connect, register, or acquire the name never disables playback or terminates the application.
- MPRIS transport methods use the same `PlaybackCommandSurface` as shell actions, shortcuts, and transport controls.
- Repeat and shuffle authority remains behind `PlaybackCommands`; MPRIS does not reconstruct succession or access its internal owner.
- Seek, volume, and now-playing state come from the coherent `PlaybackService` boundary.
- Capability queries use `PlaybackCommandSurface::isCapable`, not GTK action or widget sensitivity.
- A stale `SetPosition`, an invalid range, or a request without a current track does not mutate playback.
- Preview seek updates do not emit the protocol's final-seek signal.
- Runtime and core values never contain D-Bus object paths or file URLs.

## State model

The bridge retains D-Bus connection/object/name registrations and runtime subscriptions only while started.
Its active state means the canonical name was acquired, not merely requested.

Protocol properties are derived from current runtime and command-surface state on demand.
Metadata snapshot construction derives a stable object path from `TrackId` and copies current title, artist, album, duration, and resolved art URL.
It is not a second now-playing store.

## Commands and transitions

`start()` subscribes to playback and command availability, requests the canonical session-bus name, registers both interfaces after bus acquisition, and marks the bridge active only after name acquisition.
Repeated start is a no-op while ownership registration exists.

Known player methods execute the corresponding command.
Relative seek clamps before zero; a positive relative seek past known duration delegates to `Next`.
Absolute seek accepts only the current track path and a non-negative value no greater than known duration.
Rate remains fixed: finite nonzero writes are accepted without changing rate, while zero executes pause; non-finite writes are rejected.
Volume, shuffle, and loop writes route to their runtime authorities.

Transport, now-playing, volume, repeat, shuffle, and command-availability observations emit property-change signals for only the affected protocol fields.
Final runtime seeks emit `Seeked`; preview updates do not.

## Failure and cancellation

Bus connection, introspection, object registration, and name-ownership failures disable MPRIS for that instance and log a warning.
They do not post a user notification, change playback, or disturb an instance that already owns the name.
Unknown methods and unsupported writable properties return protocol errors.

The bridge has no independent cancellation token.
Its scoped registrations and runtime subscriptions are cleared during stop/destruction before bridge state is released.
D-Bus callbacks execute on the GTK main context and cannot block on decoder or storage work.

## Persistence and versioning

MPRIS state is not persisted.
The protocol surface follows the current exported introspection document; changes require updating the exact [MPRIS reference](../../reference/linux-gtk/mpris.md) and focused tests.

Cover-art cache files are runtime delivery artifacts rather than persisted library or application state.
Their resource behavior belongs to the [cover-art resource delivery specification](../resource/cover-art-delivery.md).

## Frontend observations

The first instance that acquires the canonical name is externally visible as Aobus.
Later instances continue as ordinary GTK applications without MPRIS.
`Raise` presents the active GTK window and `Quit` requests application quit only when their injected callbacks are installed.

## Implementation map

- [`MprisBridge.cpp`](../../../app/linux-gtk/platform/MprisBridge.cpp) owns D-Bus registration, properties, signals, and subscriptions.
- [`MprisPlaybackEndpoint.h`](../../../app/linux-gtk/platform/MprisPlaybackEndpoint.h) owns command and capability mapping without D-Bus types.
- [`MprisArtUrlCache.cpp`](../../../app/linux-gtk/platform/MprisArtUrlCache.cpp) adapts cover resources to local file URLs.
- [`MainWindow.cpp`](../../../app/linux-gtk/app/MainWindow.cpp) composes the bridge and GTK lifecycle callbacks.

## Test map

- [`MprisBridgeTest.cpp`](../../../test/unit/linux-gtk/platform/MprisBridgeTest.cpp) protects status, metadata, time, command, capability, repeat/shuffle, volume, and cover-art mapping.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Playback architecture](../../architecture/playback.md)
- [Resource delivery architecture](../../architecture/resource-delivery.md)
- [MPRIS reference](../../reference/linux-gtk/mpris.md)
- [Cover-art resource delivery](../resource/cover-art-delivery.md)
