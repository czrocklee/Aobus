---
id: architecture.resource-delivery
type: architecture
status: current
domain: resource
summary: Defines end-to-end ownership and lifetime boundaries from immutable library blobs and cover identities to GTK, TUI, CLI, playback, and MPRIS consumers.
---
# Resource delivery architecture

## Scope

This document owns the current end-to-end structural graph for library resources, with cover art as the principal consumer.
It covers content-derived `ResourceId` allocation, track cover references and primary selection, runtime byte materialization, projection and playback identity flow, GTK decode/cache/widget delivery, TUI transforms, CLI export, and MPRIS file-URL publication.

It does not own encoded-media cover extraction, track mutation transactions, exact track record layout, general presentation policy, MPRIS transport behavior, or toolkit-specific image rendering algorithms.
Those facts belong to media, library, presentation, platform, specification, and reference owners.

The subject qualifies as an end-to-end vertical slice because one immutable resource identity crosses Core storage, runtime values, application playback/projection state, asynchronous work, platform caches, widget lifetimes, terminal rendering, and an external file-URL boundary.

## System context

The [architecture landscape](README.md) classifies resource delivery as an end-to-end vertical slice refining media, library, playback, and presentation.
The [system architecture](system-overview.md) places raw resource storage in Core library, resource reads and projections in application runtime, and decoding, caching, display, terminal protocols, and MPRIS export in frontends.

```text
media file reading or YAML import
  -> TrackBuilder cover bytes
  -> ResourceStore immutable blob + ResourceId
  -> ordered Track cover references
  -> primary ResourceId in runtime rows/detail/playback state
       |-> GTK ImageCache / ThumbnailLoader / ImageWidget
       |-> TUI block preview or Kitty PNG
       |-> MPRIS cache file -> file:// URL
       `-> CLI resource list/export
```

The store contains arbitrary raw bytes and no MIME, dimension, ownership-count, or rendering metadata.
Every frontend transform therefore interprets bytes at its own platform boundary.

## Responsibilities

### Core resource identity and storage

`ResourceStore` owns raw immutable blob rows in the library database.
Creation derives the initial nonzero 32-bit id from content, verifies byte equality on collision, and probes until it finds an empty slot or identical content.
Identical bytes reuse the same id.

Tracks retain ordered `(ResourceId, PictureType)` cover entries.
Primary cover selection chooses the first `FrontCover`, otherwise the first cover, otherwise no resource.
The library model owns cover ordering and reference integrity; resource delivery owns the identity and byte route after selection.

### Runtime materialization and identity flow

`LibraryReader::loadResource()` reads under its scoped transaction and copies the transaction-borrowed span into an owned byte vector.
Runtime track rows, list/detail projections, and playback state carry only `ResourceId`, not decoded images or URLs.

This keeps platform formats out of runtime but means each current consumer initiates its own byte read and transform.
There is no shared asynchronous runtime resource service or byte cache.

### GTK image delivery

GTK `ImageCache` owns an in-process LRU of decoded pixbufs keyed by resource id plus full-size or requested physical thumbnail size.
`ThumbnailLoader` performs byte read and scaled pixbuf decoding on the shared worker pool, coalesces equal in-flight keys, returns completion on the callback executor, and permits a successful decode to populate the cache after individual interest is cancelled.

`ResourceImageController` binds a resource or detail projection to one `ImageWidget` and rejects stale thumbnail completions with a generation.
Its full-size path currently reads and decodes synchronously on the GTK thread.

### TUI delivery

TUI reads resource bytes synchronously from the runtime library when selected cover identity changes.
It decodes through stb into either half-block RGB cells or a square PNG for the Kitty graphics protocol.
The chosen transformed result is retained for the current resource id, and Kitty paint state tracks the fixed image id and terminal cell box.

### External and administrative delivery

The GTK MPRIS adapter reads the primary resource synchronously, sniffs a filename extension, writes original bytes under the user cache directory, removes stale sibling extensions, and returns a `file://` URI.
Its process-local entry is reused only while the file exists with the recorded size.

CLI resource commands expose raw ids and bytes for inspection and export without interpreting image content.

## Boundaries and dependency direction

- `ResourceStore` depends on LMDB and hashing utilities, never runtime, UIModel, or platform image libraries.
- Track and library mutation code may create/reuse resource blobs and attach ids; resource storage does not depend on track presentation or consumers.
- Runtime exposes owned bytes and stable ids without `Gdk::Pixbuf`, FTXUI cells, Kitty escapes, file URLs, MIME strings, or cache paths.
- Projections and playback state carry identity only; they do not read or decode bytes on behalf of frontends.
- GTK and TUI own decoding, scaling, display caches, and stale-view suppression.
- MPRIS file export is a GTK platform adapter and cannot become the canonical resource store.
- The same resource id always names the same bytes within one library database; ids are not portable identities across unrelated libraries.
- Cover extraction behavior belongs to the [media file reading specification](../spec/media/file-reading.md); ordered storage and mutation belong to [library](library.md); image adaptation belongs to [presentation](presentation.md).

## Data and control flow

### Ingest and publish

```text
borrowed cover bytes
  -> TrackBuilder prepare within library mutation
  -> ResourceStore::create: hash, probe, compare, create-or-reuse
  -> track cold record stores ordered resource references
  -> committed LibraryChangeSet
  -> projections/playback refresh primary ResourceId
```

### GTK thumbnail

```text
ResourceId + logical allocation + display scale
  -> physical-size cache key
  -> cache hit OR coalesced worker read/decode
  -> callback-executor cache insertion
  -> request interest + widget generation check
  -> ImageWidget source pixbuf and render policy
```

### Other current paths

```text
TUI ResourceId -> synchronous read -> stb crop/scale -> blocks or Kitty PNG
MPRIS ResourceId -> synchronous read -> extension sniff -> cache file -> file URI
CLI ResourceId -> scoped read -> raw output file
```

[RFC 0021](../rfc/0021-nonblocking-cover-art.md) proposes one asynchronous byte-read operation on the existing library task service and moves full-size GTK, TUI, and MPRIS transforms off their event-loop threads.
That proposal is not current behavior.

## Structural constraints

- `kInvalidResourceId` is zero and never names stored bytes.
- Resource bytes are immutable for the lifetime of their id.
- Resource-store spans cannot outlive their LMDB transaction; runtime consumers receive an owned copy.
- Track cover ordering and `PictureType` remain track-domain facts even when several entries deduplicate to one resource id.
- Cache keys include transform-relevant dimensions; a pixbuf too small for a requested physical size is not a hit.
- A cancelled widget/request interest cannot suppress a successful shared decode needed by another waiter.
- A destroyed or recycled widget cannot accept an older resource completion.
- External cache files are derived, replaceable artifacts and never database truth.
- Frontend decode failure does not mutate the stored resource or cover reference.

## Failure, cancellation, and lifetime boundaries

Core resource creation returns typed storage or id-exhaustion errors.
Missing reads are ordinary absence; LMDB operational faults follow the storage failure boundary.
The runtime reader copies bytes before releasing its transaction.

GTK thumbnail requests have per-interest cancellation plus a loader lifetime scope.
Worker cancellation prevents the loader from being touched after destruction; decode failure yields no pixbuf and does not poison unrelated keys.
Full-size GTK, TUI, and MPRIS paths currently perform potentially blocking read/decode/write work on their frontend thread and degrade to an empty image/URL on absence or decode/export failure.

Image byte size and decoded pixel allocation have no shared product-level budget at the resource boundary.
Frontend decoders apply their own format and dimension checks, but current architecture does not provide one cross-frontend decompression-bomb or memory-pressure policy.
RFC 0021 proposes encoded-byte, decoded-pixel, and generated-output limits while keeping transforms and caches frontend-local.

## Implementation map

- [`ResourceStore`](../../include/ao/library/ResourceStore.h), [`ResourceStore.cpp`](../../lib/library/ResourceStore.cpp), and [`CoverArt.h`](../../include/ao/library/CoverArt.h) own Core identities and references.
- [`LibraryReader::loadResource`](../../app/runtime/library/LibraryReader.cpp) owns runtime byte materialization.
- [`TrackRow.h`](../../app/include/ao/rt/TrackRow.h), [`TrackListProjection.h`](../../app/include/ao/rt/projection/TrackListProjection.h), [`TrackDetailProjection.h`](../../app/include/ao/rt/projection/TrackDetailProjection.h), and [`PlaybackState.h`](../../app/include/ao/rt/PlaybackState.h) carry identities.
- [`ImageCache`](../../app/linux-gtk/image/ImageCache.h), [`ThumbnailLoader`](../../app/linux-gtk/image/ThumbnailLoader.h), [`ResourceImageController`](../../app/linux-gtk/image/ResourceImageController.h), and [`ImageWidget`](../../app/linux-gtk/image/ImageWidget.h) own GTK delivery.
- [`CoverArt.cpp`](../../app/tui/CoverArt.cpp) and [`app/tui/App.cpp`](../../app/tui/App.cpp) own TUI transforms and paint state.
- [`MprisArtUrlCache`](../../app/linux-gtk/platform/MprisArtUrlCache.h) owns file-URL export.
- [`LibCommand.cpp`](../../app/cli/LibCommand.cpp) owns CLI inspection/export adaptation.

## Test map

- [`ResourceStoreTest.cpp`](../../test/unit/library/ResourceStoreTest.cpp) protects identity, deduplication, collisions, reads, removal, and exhaustion behavior.
- [`TrackBuilderCoverArtTest.cpp`](../../test/unit/library/TrackBuilderCoverArtTest.cpp) protects ordered references and primary selection.
- GTK image tests under [`test/unit/linux-gtk/image/`](../../test/unit/linux-gtk/image/) protect cache, coalescing, scaling, cancellation, widget generation, and render targets.
- [`PlaybackImageTest.cpp`](../../test/unit/linux-gtk/layout/components/PlaybackImageTest.cpp) and [`TrackRowCacheTest.cpp`](../../test/unit/linux-gtk/track/TrackRowCacheTest.cpp) protect runtime identity-to-widget consumers.
- [`CoverArtTest.cpp`](../../test/unit/tui/CoverArtTest.cpp) protects TUI decode and Kitty protocol transforms.
- [`MprisBridgeTest.cpp`](../../test/unit/linux-gtk/platform/MprisBridgeTest.cpp) protects cache-file export and URL publication.
- [`CliSmokeTest.cpp`](../../test/unit/cli/CliSmokeTest.cpp) protects raw resource list/export behavior.

## Related documents

- [Architecture landscape](README.md)
- [System architecture](system-overview.md)
- [Encoded media architecture](encoded-media.md)
- [Library architecture](library.md)
- [Playback architecture](playback.md)
- [Presentation architecture](presentation.md)
- [Runtime execution architecture](runtime-execution.md)
- [Cover-art resource delivery specification](../spec/resource/cover-art-delivery.md)
- [Resource blob reference](../reference/resource/blob.md)
- [Track model reference](../reference/library/model/track.md)
- [RFC 0021: non-blocking cover-art delivery](../rfc/0021-nonblocking-cover-art.md)
