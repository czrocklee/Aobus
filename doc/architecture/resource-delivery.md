---
id: architecture.resource-delivery
type: architecture
status: current
domain: resource
summary: Defines end-to-end ownership and lifetime boundaries from library descriptors and cover identities to GTK, WinUI, TUI, CLI, playback, and MPRIS consumers.
---
# Resource delivery architecture

## Scope

This document owns the current end-to-end structural graph for library resources, with cover art as the principal consumer.
It covers digest-derived `ResourceId` allocation, track cover references and primary selection, verified runtime byte reads from a derived cache or a carrier media file, projection and playback identity flow, GTK and WinUI widget delivery, TUI transforms, CLI export, and MPRIS file-URL publication.

It does not own encoded-media cover extraction, track mutation transactions, exact track record layout, general presentation policy, MPRIS transport behavior, or toolkit-specific image rendering algorithms.
Those facts belong to media, library, presentation, platform, specification, and reference owners.

The subject qualifies as an end-to-end vertical slice because one immutable resource identity crosses Core storage, runtime values, application playback/projection state, asynchronous work, platform caches, widget lifetimes, terminal rendering, and an external file-URL boundary.

## System context

The [architecture landscape](README.md) classifies resource delivery as an end-to-end vertical slice refining media, library, playback, and presentation.
The [system architecture](system-overview.md) places descriptor storage in Core library, resource byte reading and projections in application runtime, and decoding, caching, display, terminal protocols, and MPRIS export in frontends.

```text
media file reading or YAML import
  -> TrackBuilder cover bytes or declared descriptor
  -> ResourceStore descriptor (digest + length) + ResourceId
       |-> ordered Track cover references
       |    -> primary ResourceId in runtime rows/detail/playback state
       |         `-> CoreRuntime-private ResourceByteReader
       |              |-> derived cover cache (digest-keyed, verified)
       |              |-> carrier media file named by the reverse index
       |              `-> ResourceByteMemoryCache / ResourceBytes
       |                   |-> GTK ImageCache / ResourceImageLoader / CoverArtView
       |                   |-> WinUI CoverArtPresenter and SMTC
       |                   |-> TUI CoverArtLoader -> block preview or Kitty PNG
       |                   `-> MPRIS cache file -> file:// URL
       |-> YAML library export through a scoped read
       `-> CLI resource list through a scoped read, export through the same verified byte reader
```

A descriptor carries a digest and a length, and no MIME, dimension, ownership-count, or rendering metadata.
Every frontend transform therefore interprets bytes at its own platform boundary.

The content itself lives in the music files the library already indexes, so a cover read depends on a source outside the database: the derived cache, or an audio file some track references.
The library remains the authority on identity, and the source is only evidence: content is accepted if and only if it hashes to the digest.

## Responsibilities

### Core resource identity and storage

`ResourceStore` owns 36-byte descriptor rows in the library database and stores no cover bytes.
Creation hashes content with SHA-256, derives the initial nonzero 32-bit id from the digest, and probes until it finds an empty slot or an equal digest.
Identical content reuses the same id.
Rows are append-only in practice: replacing a track's covers leaves earlier descriptors in place, and each row must stay reachable along its probe chain.

Tracks retain ordered `(ResourceId, PictureType)` cover entries.
Primary cover selection chooses the first `FrontCover`, otherwise the first cover, otherwise no resource.
The library model owns cover ordering and reference integrity; resource delivery owns the identity and byte route after selection.

### Runtime byte reading and identity flow

`CoreRuntime` owns one source-private `ResourceByteReader` as the only whole-payload byte reader.
Its entry may begin on any executor; a valid request goes directly to a worker, resolves the descriptor and carrier snapshot under a short read transaction, closes that transaction, walks the derived cover cache and then each carrier file, applies the selected ceiling, and returns owned bytes on the callback executor.
Interactive delivery uses its 32 MiB path through the `ReadBytes` callable bound into a `ResourceByteMemoryCache`; CLI export calls the unbounded export path.
The two callers therefore differ in ceiling policy, not in the verified read.
YAML export still reads `ResourceStore` directly under its own scoped transaction, because a document carries descriptors rather than content.
Runtime track rows, list/detail projections, and playback state carry only `ResourceId`, not decoded images or URLs.

The reverse index that names carrier candidates is an immutable `ResourceId`-to-URI snapshot stamped with the library revision it was built from.
It is built lazily on the first cover request, rebuilt under one mutex when the stamp is stale, and published through an atomic slot, so a request holding a snapshot finishes against it while a rebuild replaces the slot, and a burst of requests on one stale stamp rebuilds once.

The derived cover cache is a digest-keyed directory outside the library, supplied to the runtime by each composition root; the runtime resolves no platform directory itself.
Entries are verified against the digest on read, installed through owner-only visibility-only atomic publication after a carrier answers, converged toward a byte budget, and evicted least-recently-used.
A cache that is absent, unwritable, or destroyed costs re-extraction and never a failed request.

`ResourceByteMemoryCache` is the `AppRuntime`-scoped read-through cache for every interactive consumer that needs encoded bytes: it coalesces equal ids, retains successful values under entry and aggregate-byte budgets, and returns immutable copyable `ResourceBytes` through its constructor-selected runtime's callback executor.
`ResourceBytes` shares owned storage across callbacks and remains valid after cache destruction or eviction.
The cache constructor accepts `async::Runtime&` plus one asynchronous `ReadBytes` function for its whole life.
`AppRuntime` constructs one cache value whose miss path calls the private interactive reader owned by its `CoreRuntime`; focused tests may construct a cache with a controlled reader directly.
WinUI, GTK, and TUI borrow that cache through `AppRuntime::resourceBytes()`, so all interactive consumers share one byte cache and it is destroyed before the owned `CoreRuntime`.
The encoded-byte cache therefore follows the `AppRuntime` rather than any individual window or consumer; closing one consumer does not flush it, and retention remains bounded at 128 entries and 128 MiB until runtime teardown.
Each spawned read retains the selected `ReadBytes` function until that flight finishes or cache destruction cancels it.
It belongs to the interactive runtime rather than the non-interactive `CoreRuntime` used by the CLI.
GTK, TUI, WinUI, and MPRIS retain their transform-specific request and cache paths, while every frontend retains its own decode and stale-result policy.

### GTK image delivery

GTK `ImageCache` owns an in-process LRU of decoded pixbufs keyed by resource id plus full-size or requested physical thumbnail size.
`ResourceImageLoader` serves both key kinds, coalesces equal in-flight keys, requests shared `ResourceBytes`, checks decoded dimensions before accepting allocation, decodes on the shared worker pool, and returns completion on the GTK callback executor.
The shared async request coalescer keeps one flight per key and ordered, independently cancellable callback interests.
Each transform flight retains its exact upstream byte-request registration until completion or clear, so cancelled UI interests do not discard cache-salvage work and a stale token cannot attach to a replacement flight.
Successful shared work may populate the cache after one or every callback interest is cancelled.

`ResourceImageController` binds a resource or detail projection to one `CoverArtView`, clears an uncached replacement immediately, and cancels its previous callback interest before replacement.
An invalid resource identity selects the configured slot placeholder instead of starting a resource read.
`CoverArtView` owns GTK transparent SVG/text placeholder rendering, scales one shared vinyl surface to the allocated size, draws the vinyl outer accent ring and muted center label from the active theme, and delegates a decoded cover to `ImageWidget`.
Group-heading, Inspector, and Now Playing layout components expose style enums in their YAML descriptors and default to `monogram`, `vinyl`, and `equalizer`.

### WinUI image delivery

Each WinUI library session has one `AppRuntime`-owned `ResourceByteMemoryCache` that is never rebound.
The memory cache uses the shared request coalescer for valid-resource reads and is shared by all Windows presenters and SMTC in that session.
A library replacement creates a new runtime and cache; it does not rebind a live cache.
`CoverArtPresenter` owns one generation-fenced selection, renders the fixed slot placeholder through XAML for an invalid identity, supplies the current Windows theme accent to vinyl rendering, and decodes valid bytes through the native image source.
It copies encoded bytes into native owning memory on a worker; the callback executor only wraps that prepared memory as a Windows random-access stream and updates XAML.
It serves realized group headings, Inspector, Now Playing, and SMTC artwork for the window's one runtime.
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

`CoverArtLoader` clears its current transform when selected cover identity changes, retains one shared byte-request interest, and performs stb decode plus block or Kitty conversion directly from `ResourceBytes::view()` on a worker.
It owns one cancellable settle task and one cancellable transform task: the settle task delays the byte request until the selection has stood still, because cancelling an interest does not unstart a read the shared memory cache already began.
Replacement retires the settle task, the byte interest, and the transform task, and publication follows a cancellation-checked callback-executor hop.
The settle window is TUI-local and shares no state with the Quick Filter debounce.
The decoder checks source dimensions and pixels before full decode and bounds generated PNG retention.
Kitty paint state separately tracks the fixed image id and terminal cell box.

### External and export delivery

The GTK MPRIS adapter validates or rewrites its derived cache file on a worker.
It sniffs a filename extension, atomically publishes owner-only original bytes under the user cache directory without durability barriers, removes stale sibling extensions only after publication, and returns a `file://` URI on the GTK callback executor.
The bridge publishes metadata without `mpris:artUrl` immediately and emits replacement metadata only when the delayed URL still belongs to the current now-playing resource.

CLI resource commands expose ids and described lengths for inspection, and export reads content through the runtime path without interpreting image content.

## Boundaries and dependency direction

- `ResourceStore` depends on LMDB and the digest utility, never runtime, UIModel, or platform image libraries.
- Track and library mutation code may create/reuse descriptors and attach ids; resource storage does not depend on track presentation or consumers.
- Core never opens a media file to satisfy a resource read; re-extraction is a runtime responsibility, because only runtime knows which files a library references and where its music root now is.
- Runtime exposes stable ids and owned bytes without `Gdk::Pixbuf`, FTXUI cells, Kitty escapes, file URLs, MIME strings, or cache paths; its `AppRuntime`-scoped memory cache may retain immutable encoded bytes but never decodes them.
- The runtime consumes a cache directory it is given and resolves none; `applicationCacheDirectory()` is called by composition roots only.
- Projections and playback state carry identity only; they do not read or decode bytes on behalf of frontends.
- GTK, WinUI, and TUI own decoding, scaling, placeholder rendering, and stale-view suppression.
- GTK and TUI own transform caches, runtime owns the shared frontend encoded-byte cache, and the platform-neutral async layer owns equal-key request coalescing, callback-interest lifetime, and exact-flight dependency retention.
- UIModel owns placeholder semantics and values, while frontend assets and toolkit code own geometry and decoding.
- MPRIS file export is a GTK platform adapter and cannot become the canonical resource store.
- The same resource id always names the same digest within one library database; ids are not portable identities across unrelated libraries, while digests are.
- Cover extraction behavior belongs to the [media file reading specification](../spec/media/file-reading.md); ordered storage and mutation belong to [library](library.md); image adaptation belongs to [presentation](presentation.md).

## Data and control flow

### Ingest and publish

```text
borrowed cover bytes or a declared descriptor
  -> TrackBuilder prepare within library mutation
  -> ResourceStore::create / getOrCreate: hash, derive key, probe, create-or-reuse
  -> track cold record stores ordered resource references
  -> committed LibraryChangeSet
  -> projections/playback refresh primary ResourceId
```

A scan reconciles a track's cover references against the file: `Changed` and `Moved` items replace the reference set with the one the file now carries. A full import restores a reference graph from a transfer document instead; nothing else writes them.

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
WinUI ResourceId -> ResourceByteMemoryCache coalesced read/cache -> worker native-memory preparation -> generation-fenced native image source or empty result
TUI ResourceId -> selection settle -> ResourceByteMemoryCache / ResourceBytes -> worker stb crop/scale -> current-task blocks or Kitty PNG
MPRIS ResourceId -> ResourceByteMemoryCache / ResourceBytes -> worker cache validation/write -> current-resource file URI
CLI ResourceId -> descriptor + carrier snapshot -> cache or carrier walk -> output file
```

## Structural constraints

- `kInvalidResourceId` is zero and never names a descriptor row.
- A descriptor's digest is immutable for the lifetime of its id, and no production path deletes a row.
- Content is accepted from any source only when it hashes to the digest; a source is evidence, never authority.
- No read transaction is held across a cache lookup or a carrier-file open; runtime consumers receive owned bytes.
- A cached runtime byte request completes synchronously inside `ResourceByteMemoryCache::request()`, so consumers establish replacement and generation state before requesting.
- Track cover ordering and `PictureType` remain track-domain facts even when several entries deduplicate to one resource id.
- Cache keys include transform-relevant dimensions; a pixbuf too small for a requested physical size is not a hit.
- A cancelled widget/request interest cannot suppress a successful shared decode needed by another waiter.
- Clearing an owner-bound request set cannot let a late completion retire or notify a replacement flight for the same key.
- A destroyed or recycled widget cannot accept an older resource completion.
- An invalid resource identity may select a frontend placeholder without creating or reading a library resource; a valid identity never falls back to that placeholder.
- External cache files, including cover cache entries, are derived, replaceable artifacts and never database truth.
- A read that no source can satisfy yields no image and never rewrites the reference.
- Frontend decode failure does not mutate the stored descriptor or cover reference.

## Failure, cancellation, and lifetime boundaries

Core resource creation returns typed storage or id-exhaustion errors.
Missing reads are ordinary absence; LMDB operational faults follow the storage failure boundary.
The runtime reader copies the descriptor and the carrier snapshot it read and closes its transaction before any cache or file I/O, so no read transaction spans either tier.

Runtime byte and GTK/MPRIS transform requests have per-interest cancellation plus an owner lifetime scope; each WinUI presenter additionally owns a generation fence and worker stream-preparation task; TUI owns one selected byte interest plus cancellable settle and transform tasks, all retired together by replacement, clearing, and destruction.
WinUI window teardown destroys SMTC and its cover-art presenters before releasing the heap-pinned session; session release resets `InteractiveBorrowers` and then its optional `RuntimeGraph`, whose direct `AppRuntime` value destroys the shared resource memory cache before the composed `CoreRuntime`; runtime shutdown joins cancelled work rather than deferring or quarantining a runtime owner.
`ResourceByteMemoryCache` destruction cancels its lifetime scope before clearing in-flight requests and retained bytes; its constructor-selected reader and callback-runtime reference then die with the cache.
Each delivery owner cancels external work before clearing its shared request coalescer.
The coalescer's flight token identifies one exact start generation, so a late completion after clear cannot match a same-key replacement.
Worker cancellation prevents a frontend owner from being touched after destruction.
Resource replacement invalidates the old callback interest, callback scope, or task before new output is published.
Absence, an over-budget payload, decode failure, or file-export failure yields no decoded resource image/URL and does not mutate stored bytes or poison unrelated cache keys.

Interactive reads reject payloads above 32 MiB, and content above that ceiling is never installed in the cover cache.
GTK and TUI reject source dimensions above 8192 or decoded images above 32,000,000 pixels before accepting a full decode.
TUI generated PNG output retains at most 8 MiB.
These delivery limits do not constrain CLI raw export or change stored bytes.

## Implementation map

- [`ResourceStore`](../../include/ao/library/ResourceStore.h), [`ResourceStore.cpp`](../../lib/library/ResourceStore.cpp), [`ResourceLayout.h`](../../include/ao/library/ResourceLayout.h), [`Sha256.h`](../../include/ao/utility/Sha256.h), and [`CoverArt.h`](../../include/ao/library/CoverArt.h) own Core identities and references.
- [`ResourceByteReader.cpp`](../../app/runtime/resource/ResourceByteReader.cpp) owns the verified cache/carrier read and carrier-index slot; [`ResourceCarrierIndex.cpp`](../../app/runtime/resource/ResourceCarrierIndex.cpp) owns the reverse index, and [`ResourceByteDiskCache.cpp`](../../app/runtime/resource/ResourceByteDiskCache.cpp) owns the derived cover cache; [`CoreRuntime.cpp`](../../app/runtime/CoreRuntime.cpp) binds interactive and export entry points, while [`LibraryYamlExporter.cpp`](../../app/runtime/library/LibraryYamlExporter.cpp) reads descriptors directly under its own transaction.
- [`ResourceByteMemoryCache`](../../app/include/ao/rt/resource/ResourceByteMemoryCache.h) and [`ResourceBytes`](../../app/include/ao/rt/resource/ResourceBytes.h) own `AppRuntime`-scoped encoded-byte coalescing, immutable ownership, retention, cancellation, and callback-executor completion.
- [`RequestCoalescer`](../../include/ao/async/RequestCoalescer.h) owns platform-neutral equal-key flight sharing, callback-interest cancellation, exact-flight dependency retention, and completion generation fencing.
- [`TrackRow.h`](../../app/include/ao/rt/TrackRow.h), [`TrackListProjection.h`](../../app/include/ao/rt/projection/TrackListProjection.h), [`TrackDetailProjection.h`](../../app/include/ao/rt/projection/TrackDetailProjection.h), and [`PlaybackState.h`](../../app/include/ao/rt/PlaybackState.h) carry identities.
- [`CoverArtPlaceholder`](../../app/include/ao/uimodel/presentation/CoverArtPlaceholder.h) owns shared presentation policy.
- [`ImageCache`](../../app/linux-gtk/image/ImageCache.h), [`ResourceImageLoader`](../../app/linux-gtk/image/ResourceImageLoader.h), [`ResourceImageController`](../../app/linux-gtk/image/ResourceImageController.h), [`CoverArtView`](../../app/linux-gtk/image/CoverArtView.h), and [`ImageWidget`](../../app/linux-gtk/image/ImageWidget.h) own GTK delivery.
- [`asset/ui/no-cover/`](../../asset/ui/no-cover/) and [`SoulMark.svg`](../../asset/brand/SoulMark.svg) own shared source geometry; [`CoverArtPresenter`](../../app/windows-winui/image/CoverArtPresenter.h) owns WinUI worker preparation and presentation, while the shared [`MemoryRandomAccessStream`](../../app/windows/include/ao/winui/MemoryRandomAccessStream.h) adapter owns Windows Runtime stream wrapping.
- [`CoverArtLoader`](../../app/tui/CoverArtLoader.h), [`CoverArt.cpp`](../../app/tui/CoverArt.cpp), and [`app/tui/App.cpp`](../../app/tui/App.cpp) own TUI delivery, transforms, and paint state.
- [`MprisArtUrlCache`](../../app/linux-gtk/platform/MprisArtUrlCache.h) owns file-URL export.
- [`LibCommand.cpp`](../../app/cli/LibCommand.cpp) owns CLI inspection/export adaptation.

## Test map

- [`ResourceStoreTest.cpp`](../../test/unit/library/ResourceStoreTest.cpp) protects identity, digest reuse, collisions, length evidence, reads, removal, and exhaustion behavior; [`Sha256Test.cpp`](../../test/unit/utility/Sha256Test.cpp) and [`ResourceLayoutTest.cpp`](../../test/unit/library/ResourceLayoutTest.cpp) protect the digest and descriptor surfaces.
- [`ResourceByteReadTest.cpp`](../../test/unit/runtime/resource/ResourceByteReadTest.cpp), [`ResourceByteReaderTest.cpp`](../../test/unit/runtime/resource/ResourceByteReaderTest.cpp), and [`ResourceByteDiskCacheTest.cpp`](../../test/unit/runtime/resource/ResourceByteDiskCacheTest.cpp) protect the two-tier walk, carrier fallback, verification, ceilings, cancellation, lazy index rebuilding, callback affinity, eviction, and cache-failure tolerance.
- [`ScanApplyOperationTest.cpp`](../../test/unit/runtime/library/ScanApplyOperationTest.cpp) protects covers as a scan fact across `New`, `Changed`, `Moved`, and `Unchanged` items.
- [`TrackBuilderCoverArtTest.cpp`](../../test/unit/library/TrackBuilderCoverArtTest.cpp) protects ordered references and primary selection.
- [`RequestCoalescerTest.cpp`](../../test/unit/async/RequestCoalescerTest.cpp) protects shared-flight ordering, interest cancellation, exact-flight dependency retention/release, reentrancy, failure rollback, clear generation fencing, and owner-independent request handles.
- [`ResourceByteMemoryCacheTest.cpp`](../../test/unit/runtime/resource/ResourceByteMemoryCacheTest.cpp) protects bounded encoded-byte retention, shared storage lifetime beyond cache destruction, application-runtime and controlled-reader paths, synchronous cache hits, retry, callback affinity, cancellation, fanout teardown, and destruction fencing against a replacement cache.
- GTK image tests under [`test/unit/linux-gtk/image/`](../../test/unit/linux-gtk/image/) protect cache, coalescing, scaling, cancellation, current-request publication, and render targets.
- [`PlaybackImageTest.cpp`](../../test/unit/linux-gtk/layout/components/PlaybackImageTest.cpp) protects the runtime identity-to-widget consumer.
- [`CoverArtPlaceholderTest.cpp`](../../test/unit/uimodel/presentation/CoverArtPlaceholderTest.cpp) protects the shared presentation policy.
- [`MemoryRandomAccessStreamTest.cpp`](../../test/unit/windows/platform/MemoryRandomAccessStreamTest.cpp) protects exact native-memory stream wrapping; native Debug and Release WinUI builds protect XAML SVG loading and presenter integration.
- [`CoverArtTest.cpp`](../../test/unit/tui/CoverArtTest.cpp) protects TUI decode and Kitty protocol transforms.
- [`MprisBridgeTest.cpp`](../../test/unit/linux-gtk/platform/MprisBridgeTest.cpp) protects cache-file export and URL publication.
- [`CliSmokeTest.cpp`](../../test/unit/cli/CliSmokeTest.cpp) protects descriptor listing, verified byte export, and absence reporting.

## Related documents

- [Architecture landscape](README.md)
- [System architecture](system-overview.md)
- [Encoded media architecture](encoded-media.md)
- [Library architecture](library.md)
- [Playback architecture](playback.md)
- [Presentation architecture](presentation.md)
- [Runtime execution architecture](runtime-execution.md)
- [Cover-art resource delivery specification](../spec/resource/cover-art-delivery.md)
- [Resource descriptor reference](../reference/resource/blob.md)
- [Decision 0010: never write to an audio file](../decision/0010-never-write-to-audio-files.md)
- [Track model reference](../reference/library/model/track.md)
