---
id: linux-gtk.mpris
---
# Linux MPRIS system-media integration

## Scope

This specification owns the shared Linux MPRIS adapter used by the GTK and TUI frontends.
It defines ownership, command and state authority, executor transfer, per-session bus lifetime, frontend identity, cover-art delivery, degradation, and teardown.
The exact D-Bus names, methods, properties, mappings, and metadata keys belong to the [MPRIS reference](../../reference/linux/mpris.md); that reference and the introspection XML in `MprisBridge.cpp` are the wire authorities.

## Code boundary

The [system architecture](../overview.md) places `ao_system_media_linux` in the application platform layer under `app/platform/media/linux/`.
Its `ao::media` implementation depends on runtime, UIModel, and GIO, and is shared by GTK and TUI without depending on either GTK or FTXUI.
Core, runtime, and UIModel have no MPRIS, GIO, D-Bus, or native system-media dependency, and the TUI does not acquire a GTK dependency.
The GTK and TUI composition roots inject their existing callback executor, `rt::PlaybackService`, selection-aware `uimodel::PlaybackActions`, host commands, and asynchronous `ResourceId`-to-URL request.

The bridge reads the coherent playback snapshot and occurrence-correlated live `elapsed()` query on that owner executor.
It executes ordinary transport commands through `PlaybackActions`; occurrence-guarded seeks, including past-end Next behavior, use `PlaybackCommands` directly.
It never calls a widget or terminal renderer.
The bridge's retained prior snapshot exists only to select changed signals: it is not a second playback projection, state registry, or command authority.

## Sessions and executor transfer

Each bridge starts an optional private GIO bus connection on a dedicated thread and private `GMainContext`.
The native context accepts only a fully validated list of `unix:path` or `unix:abstract` session-bus addresses; mixed fallback lists, unknown address fields, and non-Unix transports are rejected before connecting.
It opens the selected stream with an acquisition cancellable, performs Unix-credential `EXTERNAL` authentication itself, and verifies an address-supplied GUID against the authenticated server GUID.
The public credential-send API supplies the required credential byte; the empty authorization identity selects those kernel credentials, and this handle-free protocol surface does not negotiate Unix file descriptors.
Only then does it adopt the stream as a GDBus connection, issue cancellable `Hello`, register the interfaces, and make a cancellable no-queue `RequestName` call before activating the bridge.
It does not use GIO's implicit message-bus initializer, autolaunch a bus, reconnect, or retry after connection or ownership loss.
An explicit bridge address takes precedence, including an explicitly empty value that disables acquisition.
Otherwise a present `DBUS_SESSION_BUS_ADDRESS` is used as-is, including empty or invalid values; it is never silently replaced with another bus.
Only when that variable is unset does discovery consider an existing same-user `bus` socket under an absolute `XDG_RUNTIME_DIR`, with its path escaped for a Unix D-Bus address.
No implicit candidate leaves the adapter inert and produces an informational diagnostic; an invalid or unavailable selected bus degrades that session without affecting playback.
A `NameLost` event is accepted only from the bus daemon and only when its name field exactly matches this session's requested name.

Native method admission transfers exclusive ownership of the `GDBusMethodInvocation` into one `Executor::defer` closure.
The existing frontend callback executor then queries live runtime state, applies commands, and sends replies or signals directly through the thread-safe connection.
There is no blocking owner query, return queue, GLib polling from the frontend loop, or second executor used to mirror playback.
If retirement prevents an admitted invocation from reaching the owner, its owned invocation supplies an error while the connection remains open or is simply released after local close.

A bridge becomes active only when its requested name is acquired.
It subscribes to playback and command availability only after acquisition, and clears those subscriptions and artwork admission before asking the native session to retire.

## Command and state invariants

- MPRIS transport methods use the same runtime/UIModel command authorities as shell actions, shortcuts, and transport controls.
- Repeat and shuffle authority remains behind `PlaybackCommands`; MPRIS does not reconstruct succession or access its internal owner.
- Seek, volume, and now-playing state come from the coherent `PlaybackService` boundary.
- Capability queries use `PlaybackActions::isCapable`, not GTK actions, widgets, or terminal state.
- Runtime and Core values never contain D-Bus object paths or file URLs.
- A stale `SetPosition`, an invalid range, or a request without a current track does not mutate playback.
- `CanSeek` requires a valid current track, a nonzero playback occurrence, and a known positive duration.
- Preview seek updates do not emit the protocol's final-seek signal.

Protocol properties are derived on demand.
Metadata construction derives an occurrence-qualified object path from `TrackId` and `PlaybackOccurrenceId` and copies current title, artist, album, duration, and the current resolved art URL.
A new occurrence refreshes `Metadata` even when the track and descriptive metadata repeat.

Known player methods execute the corresponding `PlaybackActions` command, preserving its capability gate and normal queued completion.
Relative seek captures the current occurrence and signed offset, then uses queued `seekBy` with `PlaybackRelativeSeekEndBehavior::Next`.
At execution, runtime samples live elapsed and advances only for a positive offset strictly past the known endpoint, with an available successor and matching runtime/audio identity.
Without an admissible successor it is a successful no-op; other offsets use an overflow-safe guarded final seek, including an exact-end seek.
Absolute seek accepts only the current occurrence-qualified track path and a non-negative value no greater than known duration, then submits an occurrence-bearing queued seek.
Replacement or replay before execution makes either captured occurrence stale and produces no seek or `Seeked`.
Protocol success acknowledges submission, not eventual audio completion.

Rate remains fixed: finite nonzero writes are accepted without changing rate, zero executes pause, and non-finite writes are rejected.
Volume, shuffle, and loop writes route to their runtime authorities.
`Position` reads live elapsed correlated to the published occurrence, rather than the snapshot's potentially old clock anchor; when audio has advanced to a successor ahead of runtime publication, the query keeps the published anchor instead of pairing a successor position with old metadata.

Transport, occurrence, now-playing, duration, volume, repeat, shuffle, and command-availability observations emit property changes only for affected protocol fields.
Final runtime seeks emit `Seeked`; preview updates do not.

## Frontend identity and host commands

GTK keeps the canonical `org.mpris.MediaPlayer2.aobus` name, `Aobus` identity, and `aobus` desktop entry.
Only the instance that acquires that name is externally visible through MPRIS; later GTK instances continue without it.
A rejected bridge never queues or becomes the owner when the incumbent exits; retirement of the incumbent allows a newly constructed bridge to acquire the name.
Each bridge has its own connection, so rejecting or retiring one bridge cannot release another bridge's name.
GTK injects `Raise` to present its window and `Quit` to request application quit.

Each Linux TUI session asks for a well-known name formed from `org.mpris.MediaPlayer2.aobus.tui` plus a suffix derived from its private connection's bus-assigned unique name.
It publishes identity `Aobus TUI` and no `Raise` capability.
Because no TUI desktop file is installed, it omits the optional `DesktopEntry` property from introspection and `GetAll`; a direct query for that absent property returns an error.
Its `Quit` callback enters the ordinary App-owned `ExitController`, the same path as `:quit`, the quit shortcut, Ctrl-C, and handled platform signals.
The first accepted exit immediately closes media-command and artwork admission even when a submitted metadata write keeps the terminal waiting for completion.

For every host, root `Quit` queues the host callback for a later owner-executor turn and submits its empty method reply before that callback can run.
This prevents host teardown from destroying the bridge inside the invocation handler.

## Artwork delivery

`MprisArtUrlSession` retains only the current cover resource id, one request interest, one callback scope, and the last resolved URL for that same id.
A cover change invalidates the callback scope before cancelling the old interest, clears the URL, and publishes metadata without `mpris:artUrl`.
Only completion for the current resource can publish replacement metadata with a URL.

`MprisArtUrlCache` consumes the runtime's shared encoded-byte cache and writes original bytes off the owner executor.
It detects PNG, JPEG, GIF, and WebP signatures, falling back to `.img`, and publishes the file under `<cache>/aobus/mpris-art-v2/<full-sha256><extension>`.
The full content digest, not `ResourceId`, is the filename: independent handles for equal bytes converge, while different bytes never replace or purge a ResourceId-named sibling.
Publication is owner-only and same-directory atomic without a data or namespace durability barrier.

A process-memoized entry is reused only while its path is a regular file with the recorded byte size.
This remains size-only validation; MPRIS does not read the file back to hash it again.
An invalid entry is re-exported from runtime bytes.
The files are discardable same-user delivery artifacts, not persisted application or library state.
Unlike the runtime's derived cover cache, this MPRIS delivery directory has no disk-size budget or automatic eviction; distinct exported content can accumulate.
The legacy `aobus/mpris-art` directory is no longer read or written and is not removed automatically.
Either directory can be removed manually when no player or client still relies on its published file URLs; such cleanup changes no library fact.

## Failure, cancellation, and teardown

Address validation, connection, authentication, bus setup, introspection, registration, thread-start, and name-ownership failures disable MPRIS for that session and may log a warning.
They do not post a user notification, change playback, terminate the frontend, or disturb another process that owns a different name.
Unknown methods and unsupported writable properties return protocol errors.
There is no automatic recovery during that bridge lifetime.
An unexpected exception while attaching playback or artwork after acquisition first retires partial interests and joins the bus session, then propagates to the existing executor exception boundary; it is not converted into a recoverable media error.
The failed bridge stays retired, while a newly constructed bridge may acquire the released name.

Retirement revokes owner callbacks, subscriptions, and artwork interests before requesting stop on the native context.
Every network-facing acquisition stage uses the same cancellable, including stream opening, authentication reads and writes, `Hello`, and `RequestName`.
A failure before GDBus adopts the authenticated stream closes that stream directly; after adoption, initialization failure and ordinary retirement share the connection's callback-drain and close-settlement path.

The private context unregisters objects, attempts an asynchronous flush for at most 250 milliseconds, locally closes the connection, and lets connection closure release the requested name.
Flush cancellation or local close is not an unconditional guarantee that a peer received the last reply or signal.
Async close completion and observation of connection closure are separate gates; neither the close task nor a bare closed-flag check alone permits context teardown.
Native setup-call, subscription-destroy, flush, close, and queued closed-notification completions are drained on the private context, and destruction joins the native producer without requiring the frontend executor to make progress.
Already deferred owner closures are inert after callback admission closes.
TUI `--system-media=off` avoids this transport entirely.

## Build and runtime selection

Linux enables `AOBUS_BUILD_SYSTEM_MEDIA` by default.
When disabled, CMake does not construct `ao_system_media_linux`, GTK and TUI do not link it, and their system-media adapter code is absent.
The portal matrix can select the Linux GTK and system-media features independently with `./ao build --gtk on|off --system-media on|off`; [build development](../../development/build.md) defines which other portal commands share those configure options.

The TUI accepts `--system-media=auto|off` on every platform.
`auto` is best-effort and has no effect when the binary lacks Linux system-media support or no usable session bus exists.
`off` does not construct the bridge, private thread, or MPRIS artwork interests.
Windows and macOS TUI builds currently have no native system-media adapter even though they accept the portable option.

## Persistence and versioning

MPRIS state is not persisted.
The exported surface follows the current introspection document and [protocol reference](../../reference/linux/mpris.md); changes to a name, type, access mode, or mapping update both and the focused protocol tests together.
Cover-art resource behavior also follows the [cover-art delivery specification](../resource/cover-art-delivery.md).

## Implementation map

- [`MprisBridge.cpp`](../../../app/platform/media/linux/MprisBridge.cpp) owns protocol mapping, owner-executor handling, signals, and subscriptions.
- [`MprisBusSession.cpp`](../../../app/platform/media/linux/MprisBusSession.cpp) owns the private connection, native context, invocation transfer, bounded flush, local close, and native join.
- [`MprisPlaybackEndpoint.h`](../../../app/platform/media/linux/MprisPlaybackEndpoint.h) owns command and capability mapping without D-Bus types.
- [`MprisArtUrlSession.h`](../../../app/platform/media/linux/MprisArtUrlSession.h) owns current-resource correlation and callback invalidation before cancellation.
- [`MprisArtUrlCache.cpp`](../../../app/platform/media/linux/MprisArtUrlCache.cpp) owns content-addressed file-URL artifacts.
- [`MainWindow.cpp`](../../../app/linux-gtk/app/MainWindow.cpp) composes the GTK identity and host callbacks; TUI [`App.cpp`](../../../app/tui/App.cpp) composes its unique identity, runtime option, and normal exit path.
- [`app/platform/media/CMakeLists.txt`](../../../app/platform/media/CMakeLists.txt) defines `ao_system_media_linux`.

## Test map

Focused Linux integration sources live under [`test/integration/linux/media/`](../../../test/integration/linux/media):

- [`MprisBridgeTest.cpp`](../../../test/integration/linux/media/MprisBridgeTest.cpp) covers state, command, capability, and content-addressed artwork mapping.
- [`MprisPlaybackPositionTest.cpp`](../../../test/integration/linux/media/MprisPlaybackPositionTest.cpp) covers live-clock and queued occurrence-guarded positioning.
- [`MprisArtUrlSessionTest.cpp`](../../../test/integration/linux/media/MprisArtUrlSessionTest.cpp) covers request replacement, synchronous completion, cancellation ordering, and destruction.
- [`MprisBridgeIntegrationTest.cpp`](../../../test/integration/linux/media/MprisBridgeIntegrationTest.cpp) covers private-bus properties, independent identities, no-queue contention, startup rollback, replacement after retirement, and Quit.
- [`MprisBusSessionTest.cpp`](../../../test/integration/linux/media/MprisBusSessionTest.cpp) covers owned invocation disposal, repeated close/publication settlement, fragmented, closed, rejected, malformed, oversized, and stalled authentication, GUID verification, authenticated `Hello` cancellation, spoofed and genuine name loss, address rejection, and stalled-write flush deadlines.
- [`MprisSignalTest.cpp`](../../../test/integration/linux/media/MprisSignalTest.cpp) checks changed-property signal order before a later live query.
- [`TuiMprisProcessTest.cpp`](../../../test/integration/linux/media/TuiMprisProcessTest.cpp) exercises the real parser and TUI process on owned PTYs/private buses: auto/off, invalid options, child-environment discovery, acknowledged Quit, and keyboard/signal exit. It does not establish physical desktop-client behavior; its normal/ASan availability follows the [suite contract](../../development/test/test-suite.md).

## Related documents

- [Frontend composition](README.md)
- [TUI interaction](tui.md)
- [Playback architecture](../playback/README.md)
- [Resource delivery architecture](../resource/README.md)
- [MPRIS reference](../../reference/linux/mpris.md)
