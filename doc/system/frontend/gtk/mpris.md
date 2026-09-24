---
id: linux-gtk.mpris
---
# GTK MPRIS specification

## Scope

This specification owns the GTK frontend's MPRIS adapter behavior.
It defines protocol ownership, single-name policy, command and state authority, observation, signal timing, cover-art resolver boundary, degradation, and lifetime.
The exact D-Bus names, methods, properties, mappings, and metadata keys belong to the [MPRIS reference](../../../reference/linux-gtk/mpris.md).

## Code boundary

The [system architecture](../../overview.md) places MPRIS in the GTK composition leaf.
`ao::gtk::platform::MprisBridge` and `MprisPlaybackEndpoint` live entirely in `app/linux-gtk/platform/`.
Core audio and runtime remain D-Bus-free.

The bridge reads the coherent `rt::PlaybackService` snapshot and its occurrence-correlated live `elapsed()` query, and executes ordinary transport commands through `uimodel::PlaybackActions`; occurrence-guarded seeks, including their past-end Next behavior, use `PlaybackCommands` directly.
It never calls transport widgets or layout components, because those surfaces are optional and rebuildable.
GTK application lifetime enters only through injected raise and quit callbacks; cover art enters only through an injected cancellable asynchronous `ResourceId` to URL request.

## Terminology

- The **canonical name** is the one application-wide MPRIS bus name.
- The **endpoint** is the D-Bus-free command/property mapping object used by the bridge and unit tests.
- A **stale track path** is a `SetPosition` track object path whose track and playback occurrence do not identify the current runtime subject.
- A **final seek** is a runtime seek update whose mode is not preview.

## Invariants

- At most one Aobus GTK instance exports the canonical MPRIS name.
- Failure to connect, register, or acquire the name never disables playback or terminates the application.
- MPRIS transport methods use the same runtime/UIModel command authorities as shell actions, shortcuts, and transport controls.
- Repeat and shuffle authority remains behind `PlaybackCommands`; MPRIS does not reconstruct succession or access its internal owner.
- Seek, volume, and now-playing state come from the coherent `PlaybackService` boundary.
- Capability queries use `PlaybackActions::isCapable`, not GTK action or widget sensitivity.
- A stale `SetPosition`, an invalid range, or a request without a current track does not mutate playback.
- `CanSeek` requires a valid current track, a nonzero playback occurrence, and a known positive duration.
- Preview seek updates do not emit the protocol's final-seek signal.
- Runtime and core values never contain D-Bus object paths or file URLs.

## State model

The bridge retains D-Bus connection/object/name registrations and runtime subscriptions only while started.
Its active state means the canonical name was acquired, not merely requested.

Protocol properties are derived from current runtime and command-surface state on demand.
Metadata snapshot construction derives an occurrence-qualified object path from `TrackId` and `PlaybackOccurrenceId` and copies current title, artist, album, duration, and resolved art URL.
It is not a second now-playing store.
The bridge-owned `MprisArtUrlSession` retains only the current cover resource id, its request interest, a per-request callback scope, and the last resolved URL for that same id. The session, requester, cancellation callback, and completion callback are confined to the GTK main context.

## Commands and transitions

`start()` first reserves the process's single canonical-name registration, then subscribes to playback and command availability, requests the session-bus name without queuing, registers both interfaces after bus acquisition, and marks the bridge active only after name acquisition.
An overlapping bridge in the same process is rejected before subscribing or requesting the name: GIO's shared session connection requires same-name ownership and release calls to alternate.
Repeated start is a no-op while that bridge retains its process reservation.
The reservation does not make the bridge thread-safe; startup, callbacks, and teardown remain confined to the GTK main context.

Known player methods execute the corresponding `PlaybackActions` command, preserving its capability gate and ordinary queued completion. The standalone Next method has no captured position target.
Relative seek captures the current occurrence and signed offset, then uses queued `seekBy` with `PlaybackRelativeSeekEndBehavior::Next`.
At execution, the runtime samples live elapsed and advances only for a positive offset strictly past the known endpoint, with an available successor and matching runtime/audio identity.
Without an admissible successor it remains a successful no-op; other offsets use an overflow-safe guarded final seek, including an exact-end seek.
Absolute seek accepts only the current occurrence-qualified track path and a non-negative value no greater than known duration, then submits the occurrence-bearing queued `seek`.
Both commands preserve FIFO order through a busy boundary; replacement or replay before execution makes the captured occurrence stale and produces no seek or `Seeked`.
Protocol success acknowledges submission, not eventual audio completion.
Rate remains fixed: finite nonzero writes are accepted without changing rate, while zero executes pause; non-finite writes are rejected.
Volume, shuffle, and loop writes route to their runtime authorities.

Transport, occurrence, now-playing, volume, repeat, shuffle, and command-availability observations emit property-change signals for only the affected protocol fields. A new occurrence refreshes `Metadata` even when `TrackId` and descriptive metadata repeat.
Final runtime seeks emit `Seeked`; preview updates do not.
`Position` reads live elapsed correlated to the published occurrence through `PlaybackService::elapsed()`, not the snapshot's potentially old clock anchor.
If audio has advanced ahead of runtime publication, the query retains the published anchor rather than pairing a successor position with old metadata.

When now-playing cover identity changes, the bridge cancels the old URL interest, clears its published URL, and emits current metadata immediately without `mpris:artUrl`.
The cache validates or writes the derived file off the GTK thread.
A write uses same-directory visibility-only atomic publication: clients opening the current URI observe the complete old file until its complete replacement is visible, while the discardable artifact pays no data or namespace durability barrier.
The installed file remains owner-only (`0600` on POSIX and the current user plus Local System on Windows); MPRIS clients consume it within the same user session.
A completion guarded by the current callback scope stores the URL and emits `Metadata` again; closing the old scope before request cancellation suppresses older and unregister-reentrant callbacks.

## Failure and cancellation

Bus connection, introspection, object registration, and name-ownership failures disable MPRIS for that instance and log a warning.
They do not post a user notification, change playback, or disturb an instance that already owns the name.
A failed acquisition clears exported objects and playback subscriptions but retains its non-queued name token and process reservation until normal teardown; it does not become the owner when another instance exits.
Teardown releases the GIO name token before admitting another bridge in the process. The name-lost callback must not release that token reentrantly while GIO is still updating ownership state.
Unknown methods and unsupported writable properties return protocol errors.

The bridge uses scoped cancellation for the current art URL interest and closes its callback scope before cancelling that interest.
Its art interest, D-Bus registrations, and runtime subscriptions are cleared during stop/destruction before bridge state is released.
The cache owns a lifetime scope for worker validation and file export and returns only through the GTK callback executor.
D-Bus callbacks execute on the GTK main context and do not perform database reads, file validation, or file writes.

## Persistence and versioning

MPRIS state is not persisted.
The protocol surface follows the current exported introspection document; changes require updating the exact [MPRIS reference](../../../reference/linux-gtk/mpris.md) and focused tests.

Cover-art cache files are runtime delivery artifacts rather than persisted library or application state.
Their resource behavior belongs to the [cover-art resource delivery specification](../../resource/cover-art-delivery.md).

## Frontend observations

The first instance that acquires the canonical name is externally visible as Aobus.
Later instances continue as ordinary GTK applications without MPRIS.
`Raise` presents the active GTK window and `Quit` requests application quit only when their injected callbacks are installed.

## Implementation map

- [`MprisBridge.cpp`](../../../../app/linux-gtk/platform/MprisBridge.cpp) owns D-Bus registration, properties, signals, and subscriptions.
- [`MprisPlaybackEndpoint.h`](../../../../app/linux-gtk/platform/MprisPlaybackEndpoint.h) owns command and capability mapping without D-Bus types.
- [`MprisArtUrlSession.h`](../../../../app/linux-gtk/platform/MprisArtUrlSession.h) owns current-resource correlation, callback invalidation before request cancellation, and synchronous-completion cleanup on the GTK main context.
- [`MprisArtUrlCache.cpp`](../../../../app/linux-gtk/platform/MprisArtUrlCache.cpp) adapts cover resources to local file URLs.
- [`MainWindow.cpp`](../../../../app/linux-gtk/app/MainWindow.cpp) composes the bridge and GTK lifecycle callbacks.

## Test map

- [`MprisBridgeTest.cpp`](../../../../test/unit/linux-gtk/platform/MprisBridgeTest.cpp) protects status, occurrence-qualified metadata identity, time, guarded-command no-ops, capability, repeat/shuffle, volume, and cover-art mapping.
- [`MprisBusLifetimeTest.cpp`](../../../../test/unit/linux-gtk/platform/MprisBusLifetimeTest.cpp) protects real-bus pending and acquired ownership, same-process rejection, external-name contention without promotion, and replacement after retirement.
- [`MprisPlaybackPositionTest.cpp`](../../../../test/unit/linux-gtk/platform/MprisPlaybackPositionTest.cpp) protects real-clock relative positioning, busy submission, stale replay, endpoint behavior, and pending realtime succession.
- [`MprisArtUrlSessionTest.cpp`](../../../../test/unit/linux-gtk/platform/MprisArtUrlSessionTest.cpp) protects request replacement, synchronous completion, invalidation-before-cancellation, destruction, and requester-exception cleanup.

## Related documents

- [Presentation architecture](../../presentation/README.md)
- [Playback architecture](../../playback/README.md)
- [Resource delivery architecture](../../resource/README.md)
- [MPRIS reference](../../../reference/linux-gtk/mpris.md)
- [Cover-art resource delivery](../../resource/cover-art-delivery.md)
