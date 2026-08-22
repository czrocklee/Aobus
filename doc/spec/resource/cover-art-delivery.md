---
id: resource.cover-art-delivery
type: spec
status: current
domain: resource
summary: Defines descriptor creation, primary cover selection, content-addressed materialization, graphical placeholders, frontend transforms, and MPRIS export behavior.
---
# Cover-art resource delivery

## Scope

This specification defines current behavior for describing cover content by digest, attaching ordered cover references, selecting a primary cover, materializing bytes through runtime from a derived cache or a carrier media file, and delivering cover art through GTK, WinUI, TUI, MPRIS, and CLI.
The [resource descriptor reference](../../reference/resource/blob.md) owns exact ids, descriptor fields, and store operations, while the [track model](../../reference/library/model/track.md) owns the exact cover and picture-type surface.

## Code boundary

This contract spans the **Core libraries**, **application runtime**, **UIModel**, and **frontend** layers from the [system architecture](../../architecture/system-overview.md), under the [resource delivery architecture](../../architecture/resource-delivery.md).
Core owns descriptors and track references, runtime owns the source walk, encoded-byte delivery, and identity propagation, UIModel owns placeholder and stale-selection policy, and each frontend owns its transform and presentation resources.

## Terminology

- **Resource**: cover content identified by its SHA-256 digest, named inside one library by a nonzero `ResourceId` derived from that digest.
- **Descriptor**: the persisted row for a resource: its 32-byte digest and the 32-bit byte length of the content it names. The library stores no cover bytes.
- **Carrier**: an audio file a track references, whose embedded pictures may hold a resource's content.
- **Derived cover cache**: a digest-keyed directory of materialized cover files outside the library, shared by every library on the machine.
- **Cover entry**: one ordered `(ResourceId, PictureType)` track value.
- **Primary cover**: the first front cover, otherwise the first entry.
- **Full-size GTK image**: a pixbuf decoded without thumbnail-size downscaling before `ImageWidget` rendering.
- **Thumbnail key**: a resource id plus requested physical pixel size.
- **No-cover placeholder**: a frontend-rendered presentation shown by a visible graphical cover slot whose resource id is invalid.
- **Derived external artifact**: a cover cache entry, a pixbuf, or a transformed PNG that is not library truth.

## Invariants

- Content is accepted as a resource if and only if it hashes to that resource's digest, whatever source produced it.
- Equal content described in one library returns the same existing id, because the id is derived from the digest.
- A descriptor's digest never changes once written, and no single descriptor row is ever deleted.
- A stored `byteLength` is a fact about content, never an admission rule: it never decides whether content is read.
- Cover entries preserve insertion order and contain no invalid id.
- A track's cover references are a scan fact: the file the track names is the authority on which covers it has.
- No read transaction is held across a cover-cache lookup or a carrier-file open.
- Runtime byte results own their storage after the scoped read transaction ends.
- A projection or playback update changes the exposed resource id before its observers render the new image.
- A delivery cache result is valid only for the resource id and transform dimensions represented by its key.
- Missing, undecodable, stale, or cancelled valid-resource delivery produces no decoded cover image/URL rather than displaying bytes for another id.
- A no-cover placeholder never acts as a stored resource, decoded cover result, MPRIS URL, or SMTC artwork payload.
- A valid resource id always takes precedence over placeholder presentation, including while its bytes are pending or after delivery or decode failure.

## State model

The Core store maps nonzero resource ids to 36-byte descriptors.
A track stores zero or more ordered references.
Runtime rows, detail snapshots, and now-playing state hold one primary id or the invalid sentinel.

Runtime holds one immutable `ResourceId`-to-carrier-URI snapshot per library revision, built lazily on the first cover request and republished when the revision moves.
A materialization already holding a snapshot finishes against it.
The derived cover cache lives under the composition root's application cache directory, in a `cover` subdirectory whose entries are named by digest; it is shared across libraries, converged toward a 256 MiB budget, and evicted least-recently-used.
A composition root that supplies no cache directory leaves the cache inert, and every cover is then re-extracted on each in-process byte-cache miss; content whose carrier files have all gone is then not deliverable at all, because the cache is the only tier that could still hold it.

GTK maintains an LRU pixbuf cache with distinct full-size and physical-thumbnail keys plus one coalesced flight per key.
A `ResourceImageController` maintains one optional active image interest, while `CoverArtView` distinguishes empty, no-cover placeholder, and decoded-image presentation and delegates decoded-image rendering to `ImageWidget`.

Runtime shares coalesced encoded-byte loads and an at-most-128-entry cache among GTK, TUI, WinUI, and MPRIS consumers bound to one library runtime.
Each delivered `ResourceBytes` value owns immutable shared storage and remains valid after cache eviction or loader unbinding.
Each presenter retains one generation-fenced selection.
Its visible XAML cover state is hidden when there is no group or Inspector entity, remains the configured placeholder when Now Playing has no entity, uses a placeholder for an invalid id, is empty for pending or failed valid-resource delivery, and shows a decoded image for successful delivery.

TUI retains one cancellable selection-settle task, one selected-resource byte interest and transform task, transformed cover data for that id, and separate Kitty paint state for image id `1` and the last terminal box.
MPRIS retains process-local id-to-file/URL/byte-size entries and one delayed current-resource request in the bridge.

## Commands and transitions

### Create and select

Resource creation hashes content with SHA-256, derives the initial key from the first four digest bytes read big-endian, normalizes an initial zero key to `1`, and probes the complete nonzero 32-bit key space.
An empty slot creates the row; an equal digest reuses the row; a different digest advances the key with wrap from maximum to `1`.
A writer that hashed the content corrects a stored `byteLength` that disagrees with what it counted; a length merely declared by a document only fills a row that does not exist yet.

Track preparation creates or reuses every cover resource in the same library mutation that writes the track reference, from bytes it hashes or from a descriptor a document declared.
`primary()` returns the first entry whose type is `FrontCover`, otherwise the first entry, otherwise absence.

### Runtime read and propagation

`LibraryTaskService::loadResourceAsync(id, sizeLimit, stopToken)` is the only runtime read, interactive and administrative alike.
It resolves the descriptor and the carrier snapshot under a short worker-side read transaction, closes that transaction, and then walks two tiers: the derived cover cache first, then each carrier URI the snapshot names, in a stable order.
A cache entry is served only when its content hashes to the digest; an entry that fails verification is discarded rather than served.
A carrier is opened, its embedded pictures are hashed, and the first picture matching the digest answers; a carrier that cannot be read or carries no match advances to the next candidate.
Content materialized from a carrier is installed in the cover cache when the cache is enabled and the content is within the cache's per-entry maximum.
The result is returned as owned bytes on the callback executor, and the read publishes no library task progress or maintenance state.

`sizeLimit` selects the ceiling applied to materialized bytes: `Interactive` applies the 32 MiB encoded-byte limit below, and `Administrative` applies none, which is the exemption CLI raw export keeps.
An invalid id, an absent descriptor, or a walk that no source could satisfy returns an engaged result containing `nullopt`; materialized bytes above the caller's ceiling return `ValueTooLarge`; cancellation throws `OperationCancelled` and stops the walk between candidates.
A stale reference that no source can satisfy is never rewritten by a read.

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

GTK exposes those choices through `track.table.groupCoverPlaceholderStyle`, `track.coverArt.placeholderStyle`, and `playback.image.placeholderStyle`.
Unknown values fall back to the slot default and produce a diagnostic.
WinUI exposes the inspector choice through `track.coverArt.placeholderStyle` in its shell preset documents, and uses the fixed mapping wherever a slot authors nothing. Neither the group heading nor the Now Playing slot is authored, and no placeholder choice is persisted.

The shared SVG assets for `note`, `vinyl`, and `equalizer` live under `asset/ui/no-cover/`; `soul` reuses the authoritative brand mark under `asset/brand/`.
GTK packages them as GResources and WinUI packages them as application content; both packages include the brand license alongside the Soul mark.
`monogram`, the theme-colored vinyl outer ring, and the center label are generated by each toolkit and have no image asset.

### GTK full-size image

Loading an invalid id cancels the active interest and displays the configured no-cover placeholder.
A full-size cache hit is applied directly.
On a miss, the controller clears stale imagery and requests the shared loader.
The loader requests shared `ResourceBytes`, checks source dimensions, decodes through Gdk on a worker, inserts a successful current result under the full-size key, and completes on the GTK callback executor.
Absence, an over-budget source, or decode failure leaves the widget empty.
The controller reports decoded-image availability after cache lookup and every current asynchronous completion.
The persistent Now Playing cover remains visible with its placeholder when no decoded image is available, while the tooltip-surface `playback.image` remains hidden during invalid-id, pending, absent, over-budget, and decode-failure states.
A successful current decode makes that tooltip content eligible for the shell's normal delayed reveal; losing the decoded image hides the tooltip root so the shell cancels a pending reveal or closes an open tooltip.

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

When the selected primary id changes and cover display is active, TUI clears its prior transform and opens a cancellable 100-millisecond selection-settle window; the shared byte request starts only when that window elapses while the same id is still selected.
A frame that will not show artwork starts no window at all, so a detail pane whose terminal is too short for both artwork and metadata costs no read or transform.
Block mode decodes `ResourceBytes::view()` on a worker, center-crops to a square, scales to two samples per terminal row, composites alpha over the fixed background, and renders upper-half blocks after a cancellation-checked callback-executor hop.

Kitty mode decodes the same supported raster set on a worker, center-crops and scales it, encodes bounded PNG output, base64-chunks it into Kitty transmission escapes, and paints fixed image id `1` into the current cover box after the same current-task hop.
Both modes claim the same `24x12` terminal cells and render the artwork alone, without a title, separator, or border of its own inside the detail frame.
The detail frame clears its reflected cover box before every conditional artwork slot, so a frame reserving no cells leaves an invalid box; moving, hiding, replacing, or exiting deletes the previously visible Kitty image as required by paint state.

A burst of selection changes replaces the settle window each time, so the burst costs one read and one transform for the resource still selected, not one per step.
The settle window decides only whether a read is worth starting; the current-resource fence, not the delay, is what prevents a stale transform from publishing.

### MPRIS and CLI

MPRIS invalid or absent resources produce no art URL.
The cache validates a memoized file on a worker, or asynchronously reads the resource and writes original bytes there.
It sniffs PNG, JPEG, GIF, and WebP signatures, otherwise uses `.img`; it writes `<resource-id><extension>`, removes stale known sibling extensions, and returns a file URI on the GTK callback executor.
Metadata for a new now-playing resource is first published without `mpris:artUrl`; the URL completion causes replacement metadata only if that resource is still current.

CLI list reports each descriptor's id and described length, and export materializes through the same walk with no ceiling, writing the content or reporting absence and exiting nonzero.
Absence is reported as a row that does not exist or as a row no source could reproduce; the distinction is read after the walk, in its own transaction.

## Failure and cancellation

Resource create returns storage errors or `ResourceExhausted` after a complete probe without a free slot or an equal digest, and `ValueTooLarge` for content longer than `UINT32_MAX`.
Core read absence is not an error; operational storage faults follow the LMDB contract.

A cover cache that cannot be created, written, enumerated, or pruned fails no request: the caller already holds verified content, so the only consequence is a later cold miss and a budget that may stay exceeded until a pass succeeds.
Deleting the cache directory changes no library fact, and can change only what is displayed when every carrier for a digest is already gone.

GTK decode catches `Glib::Error` and publishes an empty result.
WinUI native decode catches `hresult_error`, logs the adapter diagnostic, and retains the empty valid-resource state.
An unexpected non-cancellation failure in GTK, runtime resource-byte, or MPRIS shared loading is reported once, completes the flight with the owner's empty result, and leaves the key eligible for retry.
GTK and MPRIS loader destruction and runtime resource-byte loader unbinding cancel and destroy their lifetime scopes before clearing shared request flights; runtime resource-byte unbinding also clears its byte source and cache and leaves the loader scope-free until the next binding.
Repeated resource-byte unbinding is harmless, and every later binding creates a fresh lifetime scope.
TUI destruction cancels its settle and transform tasks and its byte interest.
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
| Materialized resource bytes for GTK, WinUI, TUI, or MPRIS | 32 MiB | `ValueTooLarge`, adapted to no decoded image/URL |
| GTK or TUI source width or height | 8192 pixels | no image |
| GTK or TUI decoded source pixels | 32,000,000 | no image |
| TUI generated Kitty PNG retained bytes | 8 MiB | no image |

Limits are inclusive, and a ceiling applies to materialized bytes rather than to a descriptor's declared length.
CLI raw resource export is administrative and is not constrained by these interactive limits.
Content above the interactive ceiling is never installed in the cover cache, whichever caller materialized it.

## Persistence and versioning

Resource descriptors are durable library records with no header, MIME field, or payload.
Track cover entries persist the id and numeric picture type.
Cover cache entries, GTK pixbufs, runtime encoded-byte entries, TUI transforms, and MPRIS files are derived process/cache artifacts and can be discarded.
Placeholder SVGs and generated presentations are application UI and are never persisted in a music library.

Aobus never writes to an audio file, which is what makes a carrier a stable source; see [decision 0010](../../decision/0010-never-write-to-audio-files.md).

Changing `ResourceId` width, invalid sentinel, digest algorithm, id derivation, probe meaning, track reference layout, or picture-type values requires a library format compatibility change.
Frontend cache-key and transform changes require no library migration because derived artifacts are regenerable.

## Frontend observations

GTK and WinUI display a cover slot for every grouped track section.
Album groups may display their valid primary cover; other group types carry no representative resource and therefore display the group-heading placeholder.
GTK displays the configured slot placeholder for an invalid id, keeps the Now Playing placeholder visible while playback is idle, clears it immediately when a valid-resource load starts, and updates only through the current callback interest.
The GTK Now Playing cover tooltip appears only for a successfully decoded current image; under the [shell tooltip scheduling contract](../shell/layout-lifecycle.md#expand-and-build), an image that becomes available beneath a stationary pointer remains closed until the pointer leaves and re-enters.
WinUI displays fixed `monogram`, `vinyl`, and `equalizer` placeholders in group heading, Inspector, and Now Playing respectively, derives the vinyl outer ring and center label from the current shared theme accent, keeps the Now Playing placeholder visible without an active track, and applies the same invalid-id/pending-valid distinction through generation-fenced presenters.
TUI shows its no-cover placeholder while delivery is pending and when the resource is absent, over-budget, or undecodable; that placeholder is one compact line inside the detail frame rather than a reserved artwork panel.
MPRIS omits `mpris:artUrl` while file materialization is pending and when no valid file URL can be produced.

These degradation states do not remove or rewrite a track's cover reference.

## Implementation map

- [`Sha256.cpp`](../../../lib/utility/Sha256.cpp) and [`ResourceLayout.cpp`](../../../lib/library/ResourceLayout.cpp) own the digest facility, the descriptor bytes, and id derivation.
- [`ResourceStore.cpp`](../../../lib/library/ResourceStore.cpp), [`TrackBuilder.cpp`](../../../lib/library/TrackBuilder.cpp), and [`TrackView.cpp`](../../../lib/library/TrackView.cpp) own creation and primary selection.
- [`LibraryReader.cpp`](../../../app/runtime/library/LibraryReader.cpp) owns synchronous cover identity reads; [`LibraryTaskService.cpp`](../../../app/runtime/library/LibraryTaskService.cpp) owns the read entry point and the carrier-index slot.
- [`ResourceMaterialization.cpp`](../../../app/runtime/library/ResourceMaterialization.cpp) owns the two-tier walk, [`ResourceCarrierIndex.cpp`](../../../app/runtime/library/ResourceCarrierIndex.cpp) owns the reverse index, and [`ResourceDiskCache.cpp`](../../../app/runtime/resource/ResourceDiskCache.cpp) owns the derived cover cache.
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

- [`Sha256Test.cpp`](../../../test/unit/utility/Sha256Test.cpp) and [`ResourceLayoutTest.cpp`](../../../test/unit/library/ResourceLayoutTest.cpp) protect published digest vectors, descriptor bytes, and id derivation.
- [`ResourceStoreTest.cpp`](../../../test/unit/library/ResourceStoreTest.cpp) and [`TrackBuilderCoverArtTest.cpp`](../../../test/unit/library/TrackBuilderCoverArtTest.cpp) protect Core behavior, including a searched id collision resolved by the probe.
- [`ResourceMaterializationTest.cpp`](../../../test/unit/runtime/library/ResourceMaterializationTest.cpp) protects the walk, its ceilings, carrier fallback, cancellation, and a restored library serving a cover with no rescan; [`ResourceDiskCacheTest.cpp`](../../../test/unit/runtime/resource/ResourceDiskCacheTest.cpp) protects verification, eviction, touch throttling, and unwritable-directory tolerance.
- [`LibraryTaskServiceTest.cpp`](../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) protects lazy index construction, one rebuild per stale revision, that one stale stamp still costs one build with several workers, and the exact interactive limit.
- [`RequestCoalescerTest.cpp`](../../../test/unit/async/RequestCoalescerTest.cpp) protects shared flight, cancellation, fanout, retry, and clear-generation behavior across frontend adapters.
- [`ResourceByteCacheTest.cpp`](../../../test/unit/runtime/resource/ResourceByteCacheTest.cpp) and [`ResourceByteLoaderTest.cpp`](../../../test/unit/runtime/resource/ResourceByteLoaderTest.cpp) protect bounded retention, real and adapter-source delivery, synchronous cache hits, failure retry, callback affinity, cancellation, fanout teardown, idempotent unbinding, and rebinding.
- [`ResourceImageLoaderTest.cpp`](../../../test/unit/linux-gtk/image/ResourceImageLoaderTest.cpp), [`ImageCacheTest.cpp`](../../../test/unit/linux-gtk/image/ImageCacheTest.cpp), and [`ImageWidgetTest.cpp`](../../../test/unit/linux-gtk/image/ImageWidgetTest.cpp) protect GTK delivery, including responsive vinyl-accent geometry.
- [`TrackViewPageTest.cpp`](../../../test/unit/linux-gtk/track/TrackViewPageTest.cpp) protects the grouped-section cover slot across album and non-album presentations.
- [`PlaybackImageTest.cpp`](../../../test/unit/linux-gtk/layout/components/PlaybackImageTest.cpp) protects GTK no-cover playback presentation, decoded-image tooltip gating, authored visibility, hover timing, and action retention.
- [`CoverArtPlaceholderTest.cpp`](../../../test/unit/uimodel/presentation/CoverArtPlaceholderTest.cpp) protects style ids, slot defaults, candidate priority, semantic group monograms, and deterministic foreground colors.
- [`MemoryRandomAccessStreamTest.cpp`](../../../test/unit/windows/platform/MemoryRandomAccessStreamTest.cpp) protects exact prepared-memory stream wrapping; native Debug and Release WinUI builds protect XAML SVG loading and presenter integration.
- [`CoverArtLoaderTest.cpp`](../../../test/unit/tui/CoverArtLoaderTest.cpp) and [`CoverArtTest.cpp`](../../../test/unit/tui/CoverArtTest.cpp) protect TUI lifetime, the selection-settle window and navigation-burst cost, supported decode, limits, block preview, PNG, and Kitty escapes.
- [`MprisBridgeTest.cpp`](../../../test/unit/linux-gtk/platform/MprisBridgeTest.cpp) protects file extensions, rewriting, stale siblings, missing ids, and URL metadata.
- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) protects descriptor listing, export by materialization, and both absence reports.

## Related documents

- [Resource delivery architecture](../../architecture/resource-delivery.md)
- [Resource descriptor reference](../../reference/resource/blob.md)
- [Track model](../../reference/library/model/track.md)
- [Library database](../../reference/library/storage/database.md)
- [Media file reading](../media/file-reading.md)
- [Scan and identity](../library/runtime/scan-and-identity.md)
- [Library mutation](../library/runtime/mutation.md)
- [Decision 0010: never write to an audio file](../../decision/0010-never-write-to-audio-files.md)
- [Managed state location](../../reference/persistence/location.md)
- [GTK MPRIS specification](../linux-gtk/mpris.md) and [surface reference](../../reference/linux-gtk/mpris.md)
- [Shell layout lifecycle](../shell/layout-lifecycle.md)
