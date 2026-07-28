---
id: resource.cover-art-delivery
type: spec
status: current
domain: resource
summary: Defines resource creation, primary cover selection, bounded async materialization, graphical placeholders, frontend transforms, and MPRIS export behavior.
---
# Cover-art resource delivery

## Scope

This specification defines current behavior for storing immutable resource bytes, attaching ordered cover references, selecting a primary cover, materializing bytes through runtime, and delivering cover art through GTK, WinUI, TUI, MPRIS, and CLI.
The [resource blob reference](../../reference/resource/blob.md) owns exact ids and store operations, while the [track model](../../reference/library/model/track.md) owns the exact cover and picture-type surface.

## Code boundary

This contract spans the **Core libraries**, **application runtime**, **UIModel**, and **frontend** layers from the [system architecture](../../architecture/system-overview.md), under the [resource delivery architecture](../../architecture/resource-delivery.md).
Core owns raw blobs and track references, runtime owns scoped reads, encoded-byte delivery, and identity propagation, UIModel owns placeholder and stale-selection policy, and each frontend owns its transform and presentation resources.

## Terminology

- **Resource**: immutable raw bytes addressed by a nonzero `ResourceId` inside one library.
- **Cover entry**: one ordered `(ResourceId, PictureType)` track value.
- **Primary cover**: the first front cover, otherwise the first entry.
- **Full-size GTK image**: a pixbuf decoded without thumbnail-size downscaling before `ImageWidget` rendering.
- **Thumbnail key**: a resource id plus requested physical pixel size.
- **No-cover placeholder**: a frontend-rendered presentation shown by a visible graphical cover slot whose resource id is invalid.
- **Derived external artifact**: a cache file or transformed PNG that is not library truth.

## Invariants

- Equal resource bytes created in one library return the same existing id.
- An id's stored bytes never change in place.
- Cover entries preserve insertion order and contain no invalid id.
- Runtime byte results own their storage after the scoped read transaction ends.
- A projection or playback update changes the exposed resource id before its observers render the new image.
- A delivery cache result is valid only for the resource id and transform dimensions represented by its key.
- Missing, undecodable, stale, or cancelled valid-resource delivery produces no decoded cover image/URL rather than displaying bytes for another id.
- A no-cover placeholder never acts as a stored resource, decoded cover result, MPRIS URL, or SMTC artwork payload.
- A valid resource id always takes precedence over placeholder presentation, including while its bytes are pending or after delivery or decode failure.

## State model

The Core store maps nonzero resource ids to raw blobs.
A track stores zero or more ordered references.
Runtime rows, detail snapshots, and now-playing state hold one primary id or the invalid sentinel.

GTK maintains an LRU pixbuf cache with distinct full-size and physical-thumbnail keys plus one coalesced flight per key.
A `ResourceImageController` maintains one optional active image interest, while `CoverArtView` distinguishes empty, no-cover placeholder, and decoded-image presentation and delegates decoded-image rendering to `ImageWidget`.

Runtime shares coalesced encoded-byte loads and an at-most-128-entry cache among GTK, TUI, WinUI, and MPRIS consumers bound to one library runtime.
Each delivered `ResourceBytes` value owns immutable shared storage and remains valid after cache eviction or loader unbinding.
Each presenter retains one generation-fenced selection.
Its visible XAML cover state is hidden when there is no group or Inspector entity, remains the configured placeholder when Now Playing has no entity, uses a placeholder for an invalid id, is empty for pending or failed valid-resource delivery, and shows a decoded image for successful delivery.

TUI retains one cancellable selected-resource byte interest and transform task, transformed cover data for that id, and separate Kitty paint state for image id `1` and the last terminal box.
MPRIS retains process-local id-to-file/URL/byte-size entries and one delayed current-resource request in the bridge.

## Commands and transitions

### Create and select

Resource creation hashes bytes, normalizes an initial zero key to `1`, and probes the complete nonzero 32-bit key space.
An empty slot creates the row; equal bytes reuse the row; unequal bytes advance the key with wrap from maximum to `1`.

Track preparation creates or reuses every byte-backed cover resource in the same library mutation that writes the track reference.
`primary()` returns the first entry whose type is `FrontCover`, otherwise the first entry, otherwise absence.

### Runtime read and propagation

`LibraryTaskService::loadResourceAsync(id, stopToken)` is the only interactive runtime read: it copies under a worker-side read transaction, returns owned bytes on the callback executor, and publishes no library task progress or maintenance state.
Administrative export reads `ResourceStore` directly under its own scoped transaction and is not part of this route.
An invalid or absent id returns an engaged result containing `nullopt`; an encoded resource above 32 MiB returns `ValueTooLarge`; cancellation throws `OperationCancelled`.

`ResourceByteLoader` binds one asynchronous byte source to one callback runtime.
Its default shared and borrowed `CoreRuntime` bindings adapt `LibraryTaskService::loadResourceAsync()` to that source contract; an explicit source adapter follows the same delivery, cancellation, and caching behavior.
An unbound loader or invalid id rejects a request without invoking its callback.
On a cache miss, equal ids share one source read and each active interest receives completion on the bound callback executor.
On a cache hit, `request()` invokes the callback synchronously before returning and returns an empty request registration.
A consumer must therefore establish replacement or generation state before calling `request()`.

Track rows, list projections, detail projections, and playback state publish the selected primary id.
They do not decode or cache bytes.

### Placeholder presentation

UIModel defines five stable placeholder style ids: `monogram`, `note`, `vinyl`, `equalizer`, and `soul`.
It owns style parsing, slot defaults, identity selection, normalized monogram text and size class, and a deterministic low-saturation monogram foreground color derived from the selected identity.
Primary-text fallback retains at most one valid UTF-8 scalar, while an explicit semantic override retains at most two.
Normalization skips leading ASCII whitespace, stops at later ASCII whitespace, uppercases ASCII letters, truncates excess scalars, and produces `?` for empty or malformed input.
A one-scalar result uses the regular size class and a two-scalar result uses the compact size class.
A group heading uses its required nonempty presented primary slot as identity, uses the final two digits of a numeric primary heading as its monogram, and uses `?` for a typed missing primary value.
Its color remains derived from the complete selected heading text rather than the shortened monogram.
Toolkit renderers map the shared size class through their own font metrics and own text, SVG loading, sizing, clipping, opacity, and theme-color adaptation.
Every placeholder renders foreground-only content over a transparent surface; it does not paint a tile or rounded background.
The vinyl disc is an opaque flat-color surface with transparent space outside its circumference and in its small spindle aperture.
Its diameter occupies approximately 90% of the cover surface.
A clipped diagonal sheen affects the disc outside the center label.
One outer ring uses the current frontend theme accent at low opacity rather than the identity-derived monogram color; no concentric groove pattern is rendered.
Each toolkit draws a low-saturation center label by mixing that theme accent with a dark neutral and preserves the transparent spindle aperture.
The center label remains unprinted and its diameter is one third of the cover surface.
The equalizer uses five solid rounded bars over approximately 72 percent of the cover width and 75 percent of its height, with a shared low-saturation cyan-to-indigo gradient and no additional toolkit margin.

The slot defaults are:

| Slot | Default |
| --- | --- |
| Group heading | `monogram` |
| Inspector or track detail | `vinyl` |
| Now Playing | `equalizer` |

GTK exposes those choices through `tracks.table.groupCoverPlaceholderStyle`, `track.coverArt.placeholderStyle`, and `playback.image.placeholderStyle`.
Unknown values fall back to the slot default and produce a diagnostic.
WinUI uses the same fixed mapping in this version and exposes no placeholder setting or persistence.

The shared SVG assets for `note`, `vinyl`, and `equalizer` live under `asset/ui/no-cover/`; `soul` reuses the authoritative brand mark under `asset/brand/`.
GTK packages them as GResources and WinUI packages them as application content; both packages include the brand license alongside the Soul mark.
`monogram`, the theme-colored vinyl outer ring, and the center label are generated by each toolkit and have no image asset.

### GTK full-size image

Loading an invalid id cancels the active interest and displays the configured no-cover placeholder.
A full-size cache hit is applied directly.
On a miss, the controller clears stale imagery and requests the shared loader.
The loader requests shared `ResourceBytes`, checks source dimensions, decodes through Gdk on a worker, inserts a successful current result under the full-size key, and completes on the GTK callback executor.
Absence, an over-budget source, or decode failure leaves the widget empty.

`ImageWidget` fits and rerenders the source for logical allocation and display scale.
During allocation churn it may show a cheaper interim resample and schedules a high-quality render after the settle interval.

### GTK thumbnail

The requested physical size is at least `1` and is derived from logical size times current display scale.
Cache lookup rejects a pixbuf whose largest decoded dimension is below that size.

On a miss, an equal in-flight `(id, physical size)` request is shared.
The shared loader reads through `ResourceByteLoader` and asks Gdk to decode at scale on a worker; callback-executor completion inserts a valid pixbuf into the cache before notifying active interests.
Resetting one request deactivates only its callback interest.
Completion erases the flight before ordered callback fanout, so a callback may request the same key as new work.
The same platform-neutral coalescer contract is used by runtime encoded-byte delivery and MPRIS file-URL materialization.
Each transform flight retains its exact upstream byte-request registration until completion or clear; a stale token rejects and releases a dependency instead of attaching it to a replacement flight.

The widget controller cancels its old interest on every full-size or thumbnail load and clear, clears a recycled image on miss, and accepts completion only through its current interest.

### WinUI

Selecting no group or Inspector entity clears and hides both decoded image and placeholder.
Now Playing instead selects its configured placeholder when there is no active track.
Selecting an invalid id for an entity clears the decoded image and displays the slot's fixed no-cover placeholder.
Selecting a valid id hides the placeholder before consulting the encoded-byte cache or starting an asynchronous runtime read.
A coordinator-owned runtime `ResourceByteLoader` coalesces equal resource requests and shares its bounded encoded-byte cache among group-heading and Inspector presenters; playback uses a separate loader shared by Now Playing and SMTC artwork so a retained retiring library remains valid.
A current cached or loaded payload is copied into owning native memory on a worker; the callback executor wraps that memory as a random-access stream and replaces the image through the native XAML decoder without another application payload copy.
Absent, over-budget, or undecodable valid resources leave both the decoded image and placeholder hidden.
Selection replacement, reset, and teardown reject stale completion through generation and task lifetime.
SMTC keeps its own active request interest and current resource id while consuming the same playback-loader bytes; it does not introduce another byte cache or read workflow.

### TUI

When the selected primary id changes and cover display is active, TUI clears its prior transform and starts a cancellable shared byte request.
Block mode decodes `ResourceBytes::view()` on a worker, center-crops to a square, scales to two samples per terminal row, composites alpha over the fixed background, and renders upper-half blocks after a cancellation-checked callback-executor hop.

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
WinUI native decode catches `hresult_error`, logs the adapter diagnostic, and retains the empty valid-resource state.
An unexpected non-cancellation failure in GTK, runtime resource-byte, or MPRIS shared loading is reported once, completes the flight with the owner's empty result, and leaves the key eligible for retry.
GTK and MPRIS loader destruction and runtime resource-byte loader unbinding cancel and destroy their lifetime scopes before clearing shared request flights; runtime resource-byte unbinding also clears its byte source and cache and leaves the loader scope-free until the next binding.
Repeated resource-byte unbinding is harmless, and every later binding creates a fresh lifetime scope.
TUI destruction cancels its selected task.
Every owner access follows a cancellation-checked callback-executor transition.
Individual GTK, runtime resource-byte, and MPRIS interests may be cancelled without discarding successful shared cache work.
Lifetime cancellation may skip flight completion because the owner clears the coalescer during teardown.
A completion token names the exact flight that started the work, so a late completion after clear cannot retire or notify a same-key replacement flight.
Resource or now-playing replacement rejects stale completion through the current request interest, task, or callback-scope identity.

Unexpected async delivery exceptions go to the runtime diagnostic boundary.
Decode or file-export failure degrades to no image/URL and logs where the adapter owns diagnostics.

### Interactive limits

| Boundary | Limit | Result when exceeded |
| --- | ---: | --- |
| Encoded resource bytes for GTK, WinUI, TUI, or MPRIS | 32 MiB | `ValueTooLarge`, adapted to no decoded image/URL |
| GTK or TUI source width or height | 8192 pixels | no image |
| GTK or TUI decoded source pixels | 32,000,000 | no image |
| TUI generated Kitty PNG retained bytes | 8 MiB | no image |

Limits are inclusive.
CLI raw resource export is administrative and is not constrained by these interactive limits.

## Persistence and versioning

Resource blobs are durable library records with no header or MIME field.
Track cover entries persist the id and numeric picture type.
GTK pixbufs, runtime encoded-byte entries, TUI transforms, and MPRIS files are derived process/cache artifacts and can be discarded.
Placeholder SVGs and generated presentations are application UI and are never persisted in a music library.

Changing `ResourceId` width, invalid sentinel, hash/probe meaning, track reference layout, or picture-type values requires a library format compatibility change.
Frontend cache-key and transform changes require no library migration because derived artifacts are regenerable.

## Frontend observations

GTK and WinUI display a cover slot for every grouped track section.
Album groups may display their valid primary cover; other group types carry no representative resource and therefore display the group-heading placeholder.
GTK displays the configured slot placeholder for an invalid id, keeps the Now Playing placeholder visible while playback is idle, clears it immediately when a valid-resource load starts, and updates only through the current callback interest.
WinUI displays fixed `monogram`, `vinyl`, and `equalizer` placeholders in group heading, Inspector, and Now Playing respectively, derives the vinyl outer ring and center label from the current shared theme accent, keeps the Now Playing placeholder visible without an active track, and applies the same invalid-id/pending-valid distinction through generation-fenced presenters.
TUI shows its no-cover placeholder while delivery is pending and when the resource is absent, over-budget, or undecodable.
MPRIS omits `mpris:artUrl` while file materialization is pending and when no valid file URL can be produced.

These degradation states do not remove or rewrite a track's cover reference.

## Implementation map

- [`ResourceStore.cpp`](../../../lib/library/ResourceStore.cpp), [`TrackBuilder.cpp`](../../../lib/library/TrackBuilder.cpp), and [`TrackView.cpp`](../../../lib/library/TrackView.cpp) own creation and primary selection.
- [`LibraryReader.cpp`](../../../app/runtime/library/LibraryReader.cpp) owns synchronous cover identity reads; [`LibraryTaskService.cpp`](../../../app/runtime/library/LibraryTaskService.cpp) owns interactive byte reads.
- [`ResourceByteLoader`](../../../app/include/ao/rt/resource/ResourceByteLoader.h), [`ResourceBytes`](../../../app/include/ao/rt/resource/ResourceBytes.h), and [`ResourceByteCache`](../../../app/include/ao/rt/resource/ResourceByteCache.h) own frontend-scoped encoded-byte delivery and retention.
- [`RequestCoalescer`](../../../include/ao/async/RequestCoalescer.h) owns equal-key flight sharing, independently cancellable interests, retained upstream dependencies, and exact-flight completion fencing.
- [`CoverArtPlaceholder.h`](../../../app/include/ao/uimodel/presentation/CoverArtPlaceholder.h) owns platform-neutral placeholder policy.
- GTK image delivery lives under [`app/linux-gtk/image/`](../../../app/linux-gtk/image/), including [`CoverArtView`](../../../app/linux-gtk/image/CoverArtView.h).
- Shared graphical assets live under [`asset/ui/no-cover/`](../../../asset/ui/no-cover/); [`SoulMark.svg`](../../../asset/brand/SoulMark.svg) remains the brand authority.
- [`CoverArtPresenter`](../../../app/windows-winui/image/CoverArtPresenter.h) owns WinUI worker byte preparation and presentation; the shared [`MemoryRandomAccessStream`](../../../app/windows/include/ao/winui/MemoryRandomAccessStream.h) adapter owns native stream wrapping.
- [`CoverArtLoader.cpp`](../../../app/tui/CoverArtLoader.cpp), [`CoverArt.cpp`](../../../app/tui/CoverArt.cpp), and [`app/tui/App.cpp`](../../../app/tui/App.cpp) own TUI delivery, transform, and paint state.
- [`MprisArtUrlCache.cpp`](../../../app/linux-gtk/platform/MprisArtUrlCache.cpp) owns file-URL artifacts.
- [`LibCommand.cpp`](../../../app/cli/LibCommand.cpp) owns CLI export.

## Test map

- [`ResourceStoreTest.cpp`](../../../test/unit/library/ResourceStoreTest.cpp) and [`TrackBuilderCoverArtTest.cpp`](../../../test/unit/library/TrackBuilderCoverArtTest.cpp) protect Core behavior.
- [`RequestCoalescerTest.cpp`](../../../test/unit/async/RequestCoalescerTest.cpp) protects shared flight, cancellation, fanout, retry, and clear-generation behavior across frontend adapters.
- [`ResourceByteCacheTest.cpp`](../../../test/unit/runtime/resource/ResourceByteCacheTest.cpp) and [`ResourceByteLoaderTest.cpp`](../../../test/unit/runtime/resource/ResourceByteLoaderTest.cpp) protect bounded retention, real and adapter-source delivery, synchronous cache hits, failure retry, callback affinity, cancellation, fanout teardown, idempotent unbinding, and rebinding.
- [`ResourceImageLoaderTest.cpp`](../../../test/unit/linux-gtk/image/ResourceImageLoaderTest.cpp), [`ImageCacheTest.cpp`](../../../test/unit/linux-gtk/image/ImageCacheTest.cpp), and [`ImageWidgetTest.cpp`](../../../test/unit/linux-gtk/image/ImageWidgetTest.cpp) protect GTK delivery, including responsive vinyl-accent geometry.
- [`TrackViewPageTest.cpp`](../../../test/unit/linux-gtk/track/TrackViewPageTest.cpp) protects the grouped-section cover slot across album and non-album presentations.
- [`PlaybackImageTest.cpp`](../../../test/unit/linux-gtk/layout/components/PlaybackImageTest.cpp) protects GTK no-cover playback presentation and action retention.
- [`CoverArtPlaceholderTest.cpp`](../../../test/unit/uimodel/presentation/CoverArtPlaceholderTest.cpp) protects style ids, slot defaults, candidate priority, semantic group monograms, and deterministic foreground colors.
- [`MemoryRandomAccessStreamTest.cpp`](../../../test/unit/windows/platform/MemoryRandomAccessStreamTest.cpp) protects exact prepared-memory stream wrapping; native Debug and Release WinUI builds protect XAML SVG loading and presenter integration.
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
