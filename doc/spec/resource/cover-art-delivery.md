---
id: resource.cover-art-delivery
type: spec
status: current
domain: resource
summary: Defines resource creation, primary cover selection, runtime materialization, GTK thumbnail delivery, TUI transforms, and MPRIS export behavior.
---
# Cover-art resource delivery

## Scope

This specification defines current behavior for storing immutable resource bytes, attaching ordered cover references, selecting a primary cover, materializing bytes through runtime, and delivering cover art through GTK, TUI, MPRIS, and CLI.
The [resource blob reference](../../reference/resource/blob.md) owns exact ids and store operations, while the [track model](../../reference/library/model/track.md) owns the exact cover and picture-type surface.

## Code boundary

This contract spans the **Core libraries**, **application runtime**, and **frontend** layers from the [system architecture](../../architecture/system-overview.md), under the [resource delivery architecture](../../architecture/resource-delivery.md).
Core owns raw blobs and track references, runtime owns scoped read values and identity propagation, and each frontend owns its transform and presentation resources.

## Terminology

- **Resource**: immutable raw bytes addressed by a nonzero `ResourceId` inside one library.
- **Cover entry**: one ordered `(ResourceId, PictureType)` track value.
- **Primary cover**: the first front cover, otherwise the first entry.
- **Full-size GTK image**: a pixbuf decoded without thumbnail-size downscaling before `ImageWidget` rendering.
- **Thumbnail key**: a resource id plus requested physical pixel size.
- **Derived external artifact**: a cache file or transformed PNG that is not library truth.

## Invariants

- Equal resource bytes created in one library return the same existing id.
- An id's stored bytes never change in place.
- Cover entries preserve insertion order and contain no invalid id.
- Runtime byte results own their storage after the scoped read transaction ends.
- A projection or playback update changes the exposed resource id before its observers render the new image.
- A frontend cache result is valid only for the resource id and transform dimensions represented by its key.
- Missing, undecodable, stale, or cancelled delivery produces no image/URL rather than displaying bytes for another id.

## State model

The Core store maps nonzero resource ids to raw blobs.
A track stores zero or more ordered references.
Runtime rows, detail snapshots, and now-playing state hold one primary id or the invalid sentinel.

GTK maintains an LRU pixbuf cache with distinct full-size and physical-thumbnail keys plus one in-flight waiter set per thumbnail key.
A `ResourceImageController` maintains a monotonically increasing binding generation and optional active thumbnail interest.

TUI retains transformed cover data for the selected resource id and separate Kitty paint state for image id `1` and the last terminal box.
MPRIS retains a process-local id-to-file/URL/byte-size entry.

## Commands and transitions

### Create and select

Resource creation hashes bytes, normalizes an initial zero key to `1`, and probes the complete nonzero 32-bit key space.
An empty slot creates the row; equal bytes reuse the row; unequal bytes advance the key with wrap from maximum to `1`.

Track preparation creates or reuses every byte-backed cover resource in the same library mutation that writes the track reference.
`primary()` returns the first entry whose type is `FrontCover`, otherwise the first entry, otherwise absence.

### Runtime read and propagation

`LibraryReader::loadResource(id)` reads under the reader's existing transaction and copies the raw span into a vector.
An absent id returns `nullopt`.

Track rows, list projections, detail projections, and playback state publish the selected primary id.
They do not decode or cache bytes.

### GTK full-size image

Loading invalid id clears the widget.
A full-size cache hit is applied directly.
On a miss, the controller synchronously loads bytes, decodes a pixbuf through Gdk, inserts it under the full-size key, and sets it as the widget source; absence or decode failure clears the widget.

`ImageWidget` fits and rerenders the source for logical allocation and display scale.
During allocation churn it may show a cheaper interim resample and schedules a high-quality render after the settle interval.

### GTK thumbnail

The requested physical size is at least `1` and is derived from logical size times current display scale.
Cache lookup rejects a pixbuf whose largest decoded dimension is below that size.

On a miss, an equal in-flight `(id, physical size)` request is shared.
The worker copies resource bytes and asks Gdk to decode at scale; callback-executor completion inserts a valid pixbuf into the shared cache before notifying active interests.
Resetting one request deactivates only its callback interest.

The widget controller increments its generation on every load or clear, cancels the old interest, clears a recycled image on miss, and accepts completion only when the captured generation remains current.

### TUI

When the selected primary id changes and cover display is active, TUI synchronously loads bytes.
Block mode decodes a supported raster, center-crops to a square, scales to two samples per terminal row, composites alpha over the fixed background, and renders upper-half blocks.

Kitty mode decodes the same supported raster set, center-crops and scales it, encodes PNG, base64-chunks it into Kitty transmission escapes, and paints fixed image id `1` into the current cover box.
Moving, hiding, replacing, or exiting deletes the previously visible Kitty image as required by paint state.

### MPRIS and CLI

MPRIS invalid or absent resources produce no art URL.
The cache sniffs PNG, JPEG, GIF, and WebP signatures, otherwise uses `.img`; it writes original bytes to `<resource-id><extension>` in the MPRIS cache, removes stale known sibling extensions, and returns a file URI.
A memoized path is reused only while it is a regular file with the recorded byte size.

CLI list reports ids, and export writes the exact raw bytes of the selected resource or reports absence.

## Failure and cancellation

Resource create returns storage errors or `ResourceExhausted` after a complete probe without a free/equal slot.
Core read absence is not an error; operational storage faults follow the LMDB contract.

GTK thumbnail decode catches `Glib::Error` and publishes an empty result.
Unexpected worker exceptions resume on the callback executor and are rethrown after waiters receive the empty/decoded result according to the current implementation.
Loader destruction cancels its lifetime scope, and a post-worker executor transition prevents later access to the destroyed loader.

Full-size GTK, TUI, and MPRIS work has no current cancellation surface and runs synchronously on the calling frontend thread.
Decode or file-export failure degrades to no image/URL and logs where the adapter owns diagnostics.

## Persistence and versioning

Resource blobs are durable library records with no header or MIME field.
Track cover entries persist the id and numeric picture type.
GTK pixbufs, TUI transforms, and MPRIS files are derived process/cache artifacts and can be discarded.

Changing `ResourceId` width, invalid sentinel, hash/probe meaning, track reference layout, or picture-type values requires a library format compatibility change.
Frontend cache-key and transform changes require no library migration because derived artifacts are regenerable.

## Frontend observations

GTK clears stale imagery immediately on a thumbnail miss and updates only on a current callback generation.
TUI shows its no-cover placeholder when the resource is absent or undecodable.
MPRIS omits `mpris:artUrl` when no valid file URL can be produced.

These degradation states do not remove or rewrite a track's cover reference.

## Implementation map

- [`ResourceStore.cpp`](../../../lib/library/ResourceStore.cpp), [`TrackBuilder.cpp`](../../../lib/library/TrackBuilder.cpp), and [`TrackView.cpp`](../../../lib/library/TrackView.cpp) own creation and primary selection.
- [`LibraryReader.cpp`](../../../app/runtime/library/LibraryReader.cpp) owns runtime materialization.
- GTK image delivery lives under [`app/linux-gtk/image/`](../../../app/linux-gtk/image/).
- [`CoverArt.cpp`](../../../app/tui/CoverArt.cpp) and [`app/tui/App.cpp`](../../../app/tui/App.cpp) own TUI transform and state.
- [`MprisArtUrlCache.cpp`](../../../app/linux-gtk/platform/MprisArtUrlCache.cpp) owns file-URL artifacts.
- [`LibCommand.cpp`](../../../app/cli/LibCommand.cpp) owns CLI export.

## Test map

- [`ResourceStoreTest.cpp`](../../../test/unit/library/ResourceStoreTest.cpp) and [`TrackBuilderCoverArtTest.cpp`](../../../test/unit/library/TrackBuilderCoverArtTest.cpp) protect Core behavior.
- [`ThumbnailLoaderTest.cpp`](../../../test/unit/linux-gtk/image/ThumbnailLoaderTest.cpp), [`ImageCacheTest.cpp`](../../../test/unit/linux-gtk/image/ImageCacheTest.cpp), and [`ImageWidgetTest.cpp`](../../../test/unit/linux-gtk/image/ImageWidgetTest.cpp) protect GTK delivery.
- [`CoverArtTest.cpp`](../../../test/unit/tui/CoverArtTest.cpp) protects supported decode, block preview, PNG, and Kitty escapes.
- [`MprisBridgeTest.cpp`](../../../test/unit/linux-gtk/platform/MprisBridgeTest.cpp) protects file extensions, rewriting, stale siblings, missing ids, and URL metadata.
- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) protects raw list/export.

## Related documents

- [Resource delivery architecture](../../architecture/resource-delivery.md)
- [Resource blob reference](../../reference/resource/blob.md)
- [Track model](../../reference/library/model/track.md)
- [Library database](../../reference/library/storage/database.md)
- [Media file reading](../media/file-reading.md)
- [Library mutation](../library/runtime/mutation.md)
- [GTK MPRIS specification](../linux-gtk/mpris.md) and [surface reference](../../reference/linux-gtk/mpris.md)
- [RFC 0021: non-blocking cover-art delivery](../../rfc/0021-nonblocking-cover-art.md)
