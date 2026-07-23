---
id: resource.cover-art-delivery
type: spec
status: current
domain: resource
summary: Defines resource creation, primary cover selection, bounded async materialization, GTK/TUI transforms, and MPRIS export behavior.
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

GTK maintains an LRU pixbuf cache with distinct full-size and physical-thumbnail keys plus one coalesced flight per key.
A `ResourceImageController` maintains one optional active image interest.

TUI retains one cancellable selected-resource load, transformed cover data for that id, and separate Kitty paint state for image id `1` and the last terminal box.
MPRIS retains process-local id-to-file/URL/byte-size entries and one delayed current-resource request in the bridge.

## Commands and transitions

### Create and select

Resource creation hashes bytes, normalizes an initial zero key to `1`, and probes the complete nonzero 32-bit key space.
An empty slot creates the row; equal bytes reuse the row; unequal bytes advance the key with wrap from maximum to `1`.

Track preparation creates or reuses every byte-backed cover resource in the same library mutation that writes the track reference.
`primary()` returns the first entry whose type is `FrontCover`, otherwise the first entry, otherwise absence.

### Runtime read and propagation

`LibraryReader::loadResource(id)` remains the synchronous administrative read.
`LibraryTaskService::loadResourceAsync(id, stopToken)` is the interactive read: it copies under a worker-side read transaction, returns owned bytes on the callback executor, and publishes no library task progress or maintenance state.
An invalid or absent id returns an engaged result containing `nullopt`; an encoded resource above 32 MiB returns `ValueTooLarge`; cancellation throws `OperationCancelled`.

Track rows, list projections, detail projections, and playback state publish the selected primary id.
They do not decode or cache bytes.

### GTK full-size image

Loading invalid id clears the widget.
A full-size cache hit is applied directly.
On a miss, the controller clears stale imagery and requests the shared loader.
The loader reads owned bytes asynchronously, checks source dimensions, decodes through Gdk on a worker, inserts a successful current result under the full-size key, and completes on the GTK callback executor.
Absence, an over-budget source, or decode failure leaves the widget empty.

`ImageWidget` fits and rerenders the source for logical allocation and display scale.
During allocation churn it may show a cheaper interim resample and schedules a high-quality render after the settle interval.

### GTK thumbnail

The requested physical size is at least `1` and is derived from logical size times current display scale.
Cache lookup rejects a pixbuf whose largest decoded dimension is below that size.

On a miss, an equal in-flight `(id, physical size)` request is shared.
The shared loader reads through `LibraryTaskService` and asks Gdk to decode at scale on a worker; callback-executor completion inserts a valid pixbuf into the cache before notifying active interests.
Resetting one request deactivates only its callback interest.
Completion erases the flight before ordered callback fanout, so a callback may request the same key as new work.

The widget controller cancels its old interest on every full-size or thumbnail load and clear, clears a recycled image on miss, and accepts completion only through its current interest.

### TUI

When the selected primary id changes and cover display is active, TUI clears its prior transform and starts a cancellable asynchronous byte read.
Block mode decodes on a worker, center-crops to a square, scales to two samples per terminal row, composites alpha over the fixed background, and renders upper-half blocks after a cancellation-checked callback-executor hop.

Kitty mode decodes the same supported raster set on a worker, center-crops and scales it, encodes bounded PNG output, base64-chunks it into Kitty transmission escapes, and paints fixed image id `1` into the current cover box after the same current-task hop.
Moving, hiding, replacing, or exiting deletes the previously visible Kitty image as required by paint state.

### MPRIS and CLI

MPRIS invalid or absent resources produce no art URL.
The cache validates a memoized file on a worker, or asynchronously reads the resource and writes original bytes there.
It sniffs PNG, JPEG, GIF, and WebP signatures, otherwise uses `.img`; it writes `<resource-id><extension>`, removes stale known sibling extensions, and returns a file URI on the GTK callback executor.
Metadata for a new now-playing resource is first published without `mpris:artUrl`; the URL completion causes replacement metadata only if that resource is still current.

CLI list reports ids, and export writes the exact raw bytes of the selected resource or reports absence.

## Failure and cancellation

Resource create returns storage errors or `ResourceExhausted` after a complete probe without a free/equal slot.
Core read absence is not an error; operational storage faults follow the LMDB contract.

GTK decode catches `Glib::Error` and publishes an empty result.
GTK and MPRIS loader destruction cancels their lifetime scopes, TUI destruction cancels its selected task, and every owner access follows a cancellation-checked callback-executor transition.
Individual GTK/MPRIS interests may be cancelled without discarding successful shared cache work.
Resource or now-playing replacement rejects stale completion through the current request interest, task, or callback-scope identity.

Unexpected async workflow exceptions go to the runtime diagnostic boundary.
Decode or file-export failure degrades to no image/URL and logs where the adapter owns diagnostics.

### Interactive limits

| Boundary | Limit | Result when exceeded |
| --- | ---: | --- |
| Encoded resource bytes for GTK, TUI, or MPRIS | 32 MiB | `ValueTooLarge`, adapted to no image/URL |
| GTK or TUI source width or height | 8192 pixels | no image |
| GTK or TUI decoded source pixels | 32,000,000 | no image |
| TUI generated Kitty PNG retained bytes | 8 MiB | no image |

Limits are inclusive.
CLI raw resource export is administrative and is not constrained by these interactive limits.

## Persistence and versioning

Resource blobs are durable library records with no header or MIME field.
Track cover entries persist the id and numeric picture type.
GTK pixbufs, TUI transforms, and MPRIS files are derived process/cache artifacts and can be discarded.

Changing `ResourceId` width, invalid sentinel, hash/probe meaning, track reference layout, or picture-type values requires a library format compatibility change.
Frontend cache-key and transform changes require no library migration because derived artifacts are regenerable.

## Frontend observations

GTK clears stale imagery immediately on any image miss and updates only through the current callback interest.
TUI shows its no-cover placeholder while delivery is pending and when the resource is absent, over-budget, or undecodable.
MPRIS omits `mpris:artUrl` while file materialization is pending and when no valid file URL can be produced.

These degradation states do not remove or rewrite a track's cover reference.

## Implementation map

- [`ResourceStore.cpp`](../../../lib/library/ResourceStore.cpp), [`TrackBuilder.cpp`](../../../lib/library/TrackBuilder.cpp), and [`TrackView.cpp`](../../../lib/library/TrackView.cpp) own creation and primary selection.
- [`LibraryReader.cpp`](../../../app/runtime/library/LibraryReader.cpp) owns synchronous administrative reads; [`LibraryTaskService.cpp`](../../../app/runtime/library/LibraryTaskService.cpp) owns interactive reads.
- GTK image delivery lives under [`app/linux-gtk/image/`](../../../app/linux-gtk/image/).
- [`CoverArtLoader.cpp`](../../../app/tui/CoverArtLoader.cpp), [`CoverArt.cpp`](../../../app/tui/CoverArt.cpp), and [`app/tui/App.cpp`](../../../app/tui/App.cpp) own TUI delivery, transform, and paint state.
- [`MprisArtUrlCache.cpp`](../../../app/linux-gtk/platform/MprisArtUrlCache.cpp) owns file-URL artifacts.
- [`LibCommand.cpp`](../../../app/cli/LibCommand.cpp) owns CLI export.

## Test map

- [`ResourceStoreTest.cpp`](../../../test/unit/library/ResourceStoreTest.cpp) and [`TrackBuilderCoverArtTest.cpp`](../../../test/unit/library/TrackBuilderCoverArtTest.cpp) protect Core behavior.
- [`RequestCoalescerTest.cpp`](../../../test/unit/linux-gtk/common/RequestCoalescerTest.cpp), [`ResourceImageLoaderTest.cpp`](../../../test/unit/linux-gtk/image/ResourceImageLoaderTest.cpp), [`ImageCacheTest.cpp`](../../../test/unit/linux-gtk/image/ImageCacheTest.cpp), and [`ImageWidgetTest.cpp`](../../../test/unit/linux-gtk/image/ImageWidgetTest.cpp) protect GTK delivery.
- [`CoverArtLoaderTest.cpp`](../../../test/unit/tui/CoverArtLoaderTest.cpp) and [`CoverArtTest.cpp`](../../../test/unit/tui/CoverArtTest.cpp) protect TUI lifetime, supported decode, limits, block preview, PNG, and Kitty escapes.
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
