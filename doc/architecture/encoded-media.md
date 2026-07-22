---
id: architecture.encoded-media
type: architecture
status: current
domain: media
summary: Defines ownership, dependency direction, and borrowed-data lifetimes across encoded-media reading, reusable container parsing, library ingestion, identity, and audio decoding.
---
# Encoded media architecture

## Scope

This document owns the structural boundary from encoded media bytes through the Core `ao_media` target to its library-ingestion, audio-identity, and decoder consumers.
It defines the dependency graph, the division between file readers and reusable container primitives, the adapters at the library and audio edges, and the lifetime rules for zero-copy views.
It is a domain-system architecture because `ao_media` has an independently guarded dependency graph, multiple consumer families, and a borrowed-data lifetime model that neither consumer architecture can own alone.

It does not define exact supported extensions, field mappings, cover roles, payload ranges, malformed-input behavior, decoder output guarantees, library reconciliation, PCM execution, or stored-resource delivery.
Those facts belong to the [media file reading specification](../spec/media/file-reading.md), [supported audio files reference](../reference/media/audio-file.md), [library architecture](library.md), [playback architecture](playback.md), and [resource delivery architecture](resource-delivery.md).

## System context

The [architecture landscape](README.md) places encoded media as a domain system within the Core-library layer defined by the [system architecture](system-overview.md).
Encoded media is below application runtime, UIModel, and frontends.
The `ao_media` target provides two cooperating surfaces: `media::file` reads a supported encoded file into library-oriented evidence, while reusable MP4 and WAVE primitives expose validated byte structure to both file readers and audio decoders.
The FLAC file reader owns its metadata-block traversal directly; FLAC decoding remains inside the audio decoder and its codec library.

```text
encoded filesystem bytes
          |
          v
ao_media
  |-- media::file::File -> format readers -> Visitor / PayloadView
  |              |                              |
  |              |                              +-> runtime audio-identity workflow
  |              +-> ao::rt::readMediaTrack -> MediaTrack -> library TrackBuilder
  |
  `-- MP4 / WAVE container primitives
                 |                         |
                 +-> media::file readers   `-> ao_audio decoders -> PCM
```

The consumer edges have separate authorities:

| Boundary | Structural role | Owning architecture |
|---|---|---|
| `ao_media` | Encoded-byte ownership, recognition, validated container views, and file evidence. | This document. |
| Runtime library adapters | Combine media evidence with library commands, hashing, cancellation, and publication. | [Library](library.md), constrained by this document's view lifetimes. |
| `ao_audio` decoders | Turn encoded input into decoder-session and PCM behavior. | [Playback](playback.md), consuming media primitives without transferring decoder ownership. |
| Stored covers and external artifacts | Materialize already-imported resource identities and bytes. | [Resource delivery](resource-delivery.md). |

## Responsibilities

### Encoded-file boundary

`ao::media::file::File` owns one read-only file mapping, format dispatch, a selected private reader, and lazy cached results.
It is the public Core boundary for recognition, normalized visitor evidence, and the encoded payload used by audio identity.
Format readers own format-specific indexing, optional evidence extraction, and payload selection without depending on library records or runtime commands.

### Reusable container primitives

The MP4 atom/demux/sample-description and WAVE RIFF facilities own validated non-owning views over container structure.
They are representation mechanisms below product policy: they do not construct library tracks, choose playback succession, publish errors to users, or own decoder sessions.

FLAC metadata traversal is private to its file reader because no second consumer shares that representation.
Keeping the parser private avoids a parallel public contract and error mechanism with no architectural consumer.

File readers may compose these primitives into normalized library evidence.
Audio decoders may compose the same primitives into packet and sample access while retaining independent decoder state and PCM policy.

### Library consumer boundary

Application runtime is the composition layer allowed to know both media and library types.
`ao::rt::readMediaTrack` is the single visitor-to-`TrackBuilder` adapter.
Its `MediaTrack` result structurally retains the backing `File` for every borrowed builder view.
Scan, direct-create, and YAML-import workflows consume this adapter; their transactions, reconciliation, cancellation, and change publication remain library responsibilities.

Audio identity is a separate payload-only flow.
Runtime opens the media file, obtains a borrowed encoded payload, and passes the span to `library::readAudioIdentity`; the library target hashes bytes but neither opens nor parses media files.

### Decoder consumer boundary

Core audio decoders own file access and decode-session state required to produce PCM.
They reuse `ao_media` container primitives where useful, but they do not consume `media::file::Visitor`, construct a library track, or inherit `File`'s lazy content cache.
File recognition and decoder selection therefore remain independent capabilities rather than one shared registry.

## Boundaries and dependency direction

- `ao_media` depends on `ao_utility` and does not depend on audio, library, runtime, UIModel, or frontend code.
- `ao_audio` may depend on `ao_media`; the reverse dependency is forbidden.
- `ao_library` does not include `ao/media/file/`. Its audio-identity helper accepts caller-owned bytes instead of opening media paths.
- Application runtime may compose `ao_media` and `ao_library` and owns their private adapter.
- UIModel and frontends consume library or playback values; they do not parse containers or retain mapped-media views.
- The supported-file dispatch belongs to `media::file`; decoder dispatch belongs to `ao_audio`. Supporting ingestion does not by itself promise PCM decoding, and vice versa.
- Container primitives expose validated structure and borrowed byte ranges, not library metadata policy or decoder execution policy.

The `ao_media_boundary_check` and `ao_library_media_file_boundary_check` build targets mechanically protect the two most important forbidden directions.

## Data and control flow

### Metadata and resource ingestion

```text
path
  -> media::file::File::open
  -> required index + normalized visitor evidence
  -> ao::rt::readMediaTrack
  -> MediaTrack lifetime owner
  -> TrackBuilder views backed by the retained File
  -> library workflow preparation and mutation
  -> stored track and cover resources
```

The media boundary performs synchronous reading inside the calling library workflow and never begins a library transaction or emits runtime change events itself.
Transaction timing belongs to that workflow: direct creation and scan preparation read before opening their write transaction, while YAML transfer retains its own one-commit apply behavior.
Scan application writes the complete prepared plan in one transaction, preserving whole-plan atomicity.

### Audio identity

```text
path
  -> media::file::File::open
  -> PayloadView { borrowed bytes, file offset }
  -> runtime-owned progress and stop token
  -> library::readAudioIdentity(span, progress, stop token)
  -> identity comparison or backfill under library policy
```

The payload range is media evidence; the hash and its persistence meaning are library evidence.
Changing a payload boundary therefore crosses both authorities and requires an explicit compatibility or re-index decision.

### Audio decoding

```text
playback input
  -> ao_audio decoder selection
  -> decoder-owned file/session state
  -> ao_media container primitives where applicable
  -> encoded packets or samples
  -> decoder PCM output
  -> StreamingSource and Engine
```

Sharing container primitives does not share mappings, caches, cancellation, or lifetime between ingestion and playback.
Each decoder session owns the state needed by its execution path.

## Structural constraints

- A `media::file::File` is move-only, sequential, and non-concurrent; its const operations may populate lazy caches.
- Visitor strings, picture bytes, and payload bytes are borrowed from storage owned by the backing `File` or its cache.
- Moving `File` transfers that backing without invalidating existing views; destroying it invalidates every outstanding view.
- `MediaTrack` declares `File` before `TrackBuilder`, so reverse member destruction releases the member builder before its borrowed backing; it is move-constructible but not move-assignable because assignment would replace the backing first.
- A copied or moved-out `TrackBuilder` that still contains media-derived views must not outlive its source `MediaTrack`; URI, tag, and custom-metadata values added by callers retain their own separate owners.
- A consumer cannot retain a container view beyond the byte storage from which it was constructed.
- Required file evidence is prepared before visitor callbacks begin, so a failed required read cannot partially mutate the runtime adapter.
- Reusable container structures remain non-owning and policy-neutral; product-layer ownership is added only at the runtime library or core audio consumer edge.
- Media reading writes no source file and owns no persisted state.
- Exact field mappings and payload boundaries are reference facts, not architecture facts.

## Failure, cancellation, and lifetime boundaries

Malformed external bytes and I/O failures are recoverable outcomes at public media and decoder boundaries.
Contract violations represent programmer or internal invariant faults; they are not a replacement for validating untrusted file structure.
The [outcome channel specification](../spec/failure/outcome-channel.md) owns common result-channel behavior, while the media and decoder specifications own their observable failure classifications.

`media::file` performs synchronous caller-thread work and accepts no cancellation token.
Longer workflows add cancellation at their owner: runtime library tasks own the stop token supplied to chunked identity hashing, while decoder sessions own decode and seek cancellation under the [playback architecture](playback.md).
Cancellation cannot convert already-delivered borrowed evidence into owned data or extend its lifetime.

The runtime adapter's destruction order is the library-ingestion lifetime proof.
Decoder mappings, demux state, and packet views instead belong to each decoder session and are retired with that session before playback releases their dependencies.

## Implementation map

- [`File`](../../include/ao/media/file/File.h) and [`Visitor`](../../include/ao/media/file/Visitor.h) define the public encoded-file boundary; [`lib/media/file/`](../../lib/media/file/) contains its dispatcher and format readers.
- [`MP4 Atom`](../../include/ao/media/mp4/Atom.h), [`MP4 Demuxer`](../../include/ao/media/mp4/Demuxer.h), and [`WAVE Riff`](../../include/ao/media/wav/Riff.h) define reusable container primitives.
- [`lib/media/CMakeLists.txt`](../../lib/media/CMakeLists.txt) defines `ao_media` and enforces its forbidden dependencies.
- [`readMediaTrack` and `MediaTrack`](../../app/runtime/library/MediaTrack.h) own runtime visitor adaptation and the builder-backing lifetime.
- [`AudioIdentity`](../../include/ao/library/AudioIdentity.h), [`AudioIdentityIndexer`](../../app/runtime/library/AudioIdentityIndexer.cpp), and [`ScanPlanBuilder`](../../app/runtime/library/ScanPlanBuilder.cpp) define the payload consumer path.
- [`DecoderFactory`](../../lib/audio/DecoderFactory.cpp), [`Mp4PacketSource`](../../lib/audio/detail/Mp4PacketSource.cpp), and [`WavDecoderSession`](../../lib/audio/WavDecoderSession.cpp) illustrate the independent decoder consumer path.

## Test map

- [`test/unit/media/file/`](../../test/unit/media/file/) protects file dispatch, format evidence, payload ranges, error atomicity, caching, and view lifetime.
- [`FileTest.cpp`](../../test/integration/media/file/FileTest.cpp) protects supported formats with encoded fixtures.
- [`AtomTest.cpp`](../../test/unit/media/mp4/AtomTest.cpp), [`DemuxerTest.cpp`](../../test/unit/media/mp4/DemuxerTest.cpp), and [`RiffTest.cpp`](../../test/unit/media/wav/RiffTest.cpp) protect reusable container boundaries.
- [`MediaTrackTest.cpp`](../../test/unit/runtime/library/MediaTrackTest.cpp) protects runtime adaptation and backing lifetime.
- [`AudioIdentityTest.cpp`](../../test/unit/library/AudioIdentityTest.cpp), [`AudioIdentityIndexerTest.cpp`](../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp), and [`ScanPlanBuilderTest.cpp`](../../test/unit/runtime/library/ScanPlanBuilderTest.cpp) protect payload consumption without a library-to-reader dependency.
- Decoder-session tests under [`test/unit/audio/`](../../test/unit/audio/) and [`Mp4PacketSourceTest.cpp`](../../test/unit/audio/detail/Mp4PacketSourceTest.cpp) protect media-primitive consumption from the audio side.

## Related documents

- [Architecture landscape](README.md)
- [System architecture](system-overview.md)
- [Library architecture](library.md)
- [Playback architecture](playback.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Resource delivery architecture](resource-delivery.md)
- [Media file reading specification](../spec/media/file-reading.md)
- [Supported audio files reference](../reference/media/audio-file.md)
- [Library scan and audio identity specification](../spec/library/runtime/scan-and-identity.md)
- [Decoder session specification](../spec/playback/decoder-session.md)
