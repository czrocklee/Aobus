---
id: architecture.resource-delivery
type: architecture
status: current
domain: resource
summary: Defines end-to-end ownership and lifetime boundaries from immutable library blobs and cover identities to GTK, WinUI, TUI, CLI, playback, and MPRIS consumers.
---
# Resource delivery architecture

## Scope

This document owns the current end-to-end structural graph for library resources, with cover art as the principal consumer.
It covers content-derived `ResourceId` allocation, track cover references and primary selection, runtime byte materialization, projection and playback identity flow, GTK and WinUI widget delivery, TUI transforms, CLI export, and MPRIS file-URL publication.

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
       |-> ordered Track cover references
       |    -> primary ResourceId in runtime rows/detail/playback state
       |         `-> LibraryTaskService owned-byte read
       |              |-> GTK ImageCache / ResourceImageLoader / CoverArtView
       |              |-> WinUI WindowsCoverArtLoader / CoverArtPresenter
       |              |-> TUI CoverArtLoader -> block preview or Kitty PNG
       |              `-> MPRIS cache file -> file:// URL
       |-> YAML library export through a scoped read
       `-> CLI resource list/export through a scoped read
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

`LibraryTaskService::loadResourceAsync()` is the only interactive materialization boundary: it enters on the callback executor, copies immutable bytes under a worker-side read transaction, rejects encoded payloads above 32 MiB, and returns owned bytes on the callback executor.
Administrative export is not routed through it: YAML export and CLI resource export each read `ResourceStore` directly under their own scoped transaction.
Runtime track rows, list/detail projections, and playback state carry only `ResourceId`, not decoded images or URLs.

The task service does not cache, decode, publish maintenance progress, or introduce a resource-state owner.
Each frontend retains its own request lifetime, transform, cache, and stale-result policy.

### GTK image delivery

GTK `ImageCache` owns an in-process LRU of decoded pixbufs keyed by resource id plus full-size or requested physical thumbnail size.
`ResourceImageLoader` serves both key kinds, coalesces equal in-flight keys, reads through `LibraryTaskService`, checks decoded dimensions before accepting allocation, decodes on the shared worker pool, and returns completion on the GTK callback executor.
The GTK request coalescer keeps one flight per key and ordered, independently cancellable callback interests.
Successful shared work may populate the cache after one or every callback interest is cancelled.

`ResourceImageController` binds a resource or detail projection to one `CoverArtView`, clears an uncached replacement immediately, and cancels its previous callback interest before replacement.
An invalid resource identity selects the configured slot placeholder instead of starting a resource read.
`CoverArtView` owns GTK transparent SVG/text placeholder rendering, scales one shared vinyl surface to the allocated size, draws the vinyl outer accent ring and muted center label from the active theme, and delegates a decoded cover to `ImageWidget`.
Group-heading, Inspector, and Now Playing layout components expose style enums in their YAML descriptors and default to `monogram`, `vinyl`, and `equalizer`.

### WinUI image delivery

WinUI `WindowsCoverArtLoader` owns coalesced valid-resource reads and a bounded encoded-byte cache shared by presenters on the same runtime.
`CoverArtPresenter` owns one generation-fenced selection, renders the fixed slot placeholder through XAML for an invalid identity, supplies the current Windows theme accent to vinyl rendering, and decodes valid bytes through the native image source.
The library loader serves realized group headings and Inspector; a playback-runtime loader serves Now Playing so retained playback can outlive library replacement.
No-entity state hides group-heading and Inspector cover surfaces, while the Now Playing surface retains its configured placeholder.
Valid-resource loading or failure leaves the corresponding surface empty.

### Shared placeholder policy

UIModel defines the five style identities, slot defaults, candidate-text reduction, normalized UTF-8 monogram, semantic regular/compact size class, and deterministic low-saturation monogram foreground color.
It does not depend on GTK, XAML, SVG decoders, fonts, drawing APIs, or packaged asset paths.
GTK and WinUI render the same presentation values as foreground-only content over a transparent placeholder surface.
The monogram color remains stable for one content identity; the vinyl outer ring and muted center label instead follow the frontend's current global theme accent and are not identity-colored.
Each toolkit sizes the unprinted center label to one third of the cover diameter and retains a small transparent spindle aperture.
The repository owns the `note`, `vinyl`, and `equalizer` SVG geometry in `asset/ui/no-cover/` and reuses the brand-owned Soul mark for `soul`; each frontend declares platform-specific packaging from those source assets and bundles the Soul brand license with the mark.

### TUI delivery

`CoverArtLoader` clears its current transform when selected cover identity changes, reads bytes asynchronously, and performs stb decode plus block or Kitty conversion on a worker.
It owns one cancellable task; replacement retires that task, and publication follows a cancellation-checked callback-executor hop.
The decoder checks source dimensions and pixels before full decode and bounds generated PNG retention.
Kitty paint state separately tracks the fixed image id and terminal cell box.

### External and administrative delivery

The GTK MPRIS adapter validates or materializes its derived cache file on a worker.
It sniffs a filename extension, writes original bytes under the user cache directory, removes stale sibling extensions, and returns a `file://` URI on the GTK callback executor.
The bridge publishes metadata without `mpris:artUrl` immediately and emits replacement metadata only when the delayed URL still belongs to the current now-playing resource.

CLI resource commands expose raw ids and bytes for inspection and export without interpreting image content.

## Boundaries and dependency direction

- `ResourceStore` depends on LMDB and hashing utilities, never runtime, UIModel, or platform image libraries.
- Track and library mutation code may create/reuse resource blobs and attach ids; resource storage does not depend on track presentation or consumers.
- Runtime exposes owned bytes and stable ids without `Gdk::Pixbuf`, FTXUI cells, Kitty escapes, file URLs, MIME strings, or cache paths.
- Projections and playback state carry identity only; they do not read or decode bytes on behalf of frontends.
- GTK, WinUI, and TUI own decoding, scaling, display caches, placeholder rendering, and stale-view suppression.
- UIModel owns placeholder semantics and values, while frontend assets and toolkit code own geometry and decoding.
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

### GTK image

```text
ResourceId + logical allocation + display scale
  -> physical-size cache key
  -> cache hit OR coalesced async byte read and worker decode
  -> callback-executor cache insertion
  -> current widget request interest
  -> CoverArtView decoded-image layer -> ImageWidget render policy
```

### Other paths

```text
WinUI ResourceId -> shared coalesced byte load/cache -> generation-fenced presenter -> native image source or empty result
TUI ResourceId -> cancellable async owned bytes -> worker stb crop/scale -> current-task blocks or Kitty PNG
MPRIS ResourceId -> async owned bytes -> worker cache validation/write -> current-resource file URI
CLI ResourceId -> scoped read -> raw output file
```

## Structural constraints

- `kInvalidResourceId` is zero and never names stored bytes.
- Resource bytes are immutable for the lifetime of their id.
- Resource-store spans cannot outlive their LMDB transaction; runtime consumers receive an owned copy.
- Track cover ordering and `PictureType` remain track-domain facts even when several entries deduplicate to one resource id.
- Cache keys include transform-relevant dimensions; a pixbuf too small for a requested physical size is not a hit.
- A cancelled widget/request interest cannot suppress a successful shared decode needed by another waiter.
- A destroyed or recycled widget cannot accept an older resource completion.
- An invalid resource identity may select a frontend placeholder without creating or reading a library resource; a valid identity never falls back to that placeholder.
- External cache files are derived, replaceable artifacts and never database truth.
- Frontend decode failure does not mutate the stored resource or cover reference.

## Failure, cancellation, and lifetime boundaries

Core resource creation returns typed storage or id-exhaustion errors.
Missing reads are ordinary absence; LMDB operational faults follow the storage failure boundary.
The runtime reader copies bytes before releasing its transaction.

GTK, WinUI, and MPRIS shared requests have per-interest cancellation plus a loader lifetime scope; each WinUI presenter additionally owns a generation fence; TUI owns one cancellable selected-resource task.
Worker cancellation prevents a frontend owner from being touched after destruction.
Resource replacement invalidates the old callback interest, callback scope, or task before new output is published.
Absence, an over-budget payload, decode failure, or file-export failure yields no decoded resource image/URL and does not mutate stored bytes or poison unrelated cache keys.

Interactive reads reject encoded payloads above 32 MiB before copying them out of storage.
GTK and TUI reject source dimensions above 8192 or decoded images above 32,000,000 pixels before accepting a full decode.
TUI generated PNG output retains at most 8 MiB.
These delivery limits do not constrain CLI raw export or change stored bytes.

## Implementation map

- [`ResourceStore`](../../include/ao/library/ResourceStore.h), [`ResourceStore.cpp`](../../lib/library/ResourceStore.cpp), and [`CoverArt.h`](../../include/ao/library/CoverArt.h) own Core identities and references.
- [`LibraryTaskService::loadResourceAsync`](../../app/runtime/library/LibraryTaskService.cpp) owns interactive materialization; [`LibraryYamlExporter.cpp`](../../app/runtime/library/LibraryYamlExporter.cpp) and CLI export read `ResourceStore` directly under their own transaction.
- [`TrackRow.h`](../../app/include/ao/rt/TrackRow.h), [`TrackListProjection.h`](../../app/include/ao/rt/projection/TrackListProjection.h), [`TrackDetailProjection.h`](../../app/include/ao/rt/projection/TrackDetailProjection.h), and [`PlaybackState.h`](../../app/include/ao/rt/PlaybackState.h) carry identities.
- [`CoverArtPlaceholder`](../../app/include/ao/uimodel/presentation/CoverArtPlaceholder.h) owns shared presentation policy.
- [`ImageCache`](../../app/linux-gtk/image/ImageCache.h), [`ResourceImageLoader`](../../app/linux-gtk/image/ResourceImageLoader.h), [`ResourceImageController`](../../app/linux-gtk/image/ResourceImageController.h), [`CoverArtView`](../../app/linux-gtk/image/CoverArtView.h), and [`ImageWidget`](../../app/linux-gtk/image/ImageWidget.h) own GTK delivery.
- [`asset/ui/no-cover/`](../../asset/ui/no-cover/) and [`SoulMark.svg`](../../asset/brand/SoulMark.svg) own shared source geometry; [`WindowsCoverArtLoader`](../../app/windows-winui/image/WindowsCoverArtLoader.h) and [`CoverArtPresenter`](../../app/windows-winui/image/CoverArtPresenter.h) own WinUI delivery.
- [`CoverArtLoader`](../../app/tui/CoverArtLoader.h), [`CoverArt.cpp`](../../app/tui/CoverArt.cpp), and [`app/tui/App.cpp`](../../app/tui/App.cpp) own TUI delivery, transforms, and paint state.
- [`MprisArtUrlCache`](../../app/linux-gtk/platform/MprisArtUrlCache.h) owns file-URL export.
- [`LibCommand.cpp`](../../app/cli/LibCommand.cpp) owns CLI inspection/export adaptation.

## Test map

- [`ResourceStoreTest.cpp`](../../test/unit/library/ResourceStoreTest.cpp) protects identity, deduplication, collisions, reads, removal, and exhaustion behavior.
- [`TrackBuilderCoverArtTest.cpp`](../../test/unit/library/TrackBuilderCoverArtTest.cpp) protects ordered references and primary selection.
- [`RequestCoalescerTest.cpp`](../../test/unit/linux-gtk/common/RequestCoalescerTest.cpp) protects shared-flight ordering, interest cancellation, reentrancy, failure rollback, and owner-independent request handles.
- GTK image tests under [`test/unit/linux-gtk/image/`](../../test/unit/linux-gtk/image/) protect cache, coalescing, scaling, cancellation, current-request publication, and render targets.
- [`PlaybackImageTest.cpp`](../../test/unit/linux-gtk/layout/components/PlaybackImageTest.cpp) protects the runtime identity-to-widget consumer.
- [`CoverArtPlaceholderTest.cpp`](../../test/unit/uimodel/presentation/CoverArtPlaceholderTest.cpp) protects the shared presentation policy.
- Native Debug and Release WinUI builds protect XAML SVG loading and presenter integration.
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
