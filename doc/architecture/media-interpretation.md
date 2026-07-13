---
id: architecture.media-interpretation
type: architecture
status: current
domain: media
summary: Defines ownership and dependency boundaries for encoded-container parsing, tag interpretation, payload extraction, and downstream consumers.
---
# Media interpretation architecture

## Scope

This document owns the current structural graph for interpreting supported encoded audio files.
It covers reusable container parsing in `ao_media`, tag and technical-property interpretation in `ao_tag`, encoded-payload extraction, the library and runtime consumers of those results, and the decoder's use of shared container primitives.

It does not own library scan reconciliation, stored track fields, audio decoding state, playback succession, or cover-resource delivery after an interpreted cover enters a library mutation.
Those concerns belong to library, playback, resource-delivery, and exact reference owners.

The subject qualifies as a domain-system architecture because it has an independent parser ownership and borrowed-data lifetime graph used by both library ingestion and audio decoding.

## System context

The [architecture landscape](README.md) classifies media interpretation as a domain system.
The [system architecture](system-overview.md) places `ao_media`, `ao_tag`, `ao_library`, and `ao_audio` in Core libraries, while application runtime coordinates scan, import, export, and editing workflows above them.

The declared build graph and the semantic source graph currently differ:

```text
declared CMake graph
  ao_tag -> ao_media -> ao_utility
  ao_library -> ao_lmdb

semantic source graph
  ao_tag -> ao_library::TrackBuilder
  ao_library::AudioIdentity -> ao_tag::TagFile
  ao_audio -> ao_media container parsers
```

`TagFile.h` includes `TrackBuilder.h`, and `AudioIdentity.cpp` opens `TagFile`.
The result is a Core-library semantic cycle between tag interpretation and library representation even though the CMake target links do not declare it.
[RFC 0020](../rfc/0020-decoupled-media-interpretation.md) proposes a representation-neutral interpretation boundary; the cycle above remains the current architecture until that RFC is implemented.

## Responsibilities

### Media parser primitives

`ao_media` owns byte-level FLAC metadata-block, MP4 atom/demux, and RIFF/WAVE parsing primitives.
These types describe container evidence and do not own library identities, runtime tasks, or frontend behavior.

### Tag-file interpretation

`ao_tag::TagFile` owns supported-extension recognition, read-only file mapping, per-format reader dispatch, metadata and technical-property interpretation into a `library::TrackBuilder`, cover extraction, and the encoded-audio payload range.
Format implementations under `lib/tag/` combine tag-specific readers with shared `ao_media` primitives.

The extension dispatch is the recognition authority for library scanning.
The exact extensions, tag mappings, cover roles, codec values, and payload ranges belong to the [supported audio files reference](../reference/media/audio-file.md).

### Library ingestion and identity

Runtime scan, YAML transfer, and metadata-edit workflows open `TagFile` to obtain a baseline `TrackBuilder` or determine whether a path is supported.
The library identity helper hashes the borrowed encoded payload in chunks and returns an `AudioIdentity`; scan planning and the identity indexer persist that evidence through library mutations.

Library architecture owns reconciliation, mutation, revision publication, and stored identity semantics after interpretation.
Media interpretation owns how external bytes become the candidate and payload evidence those workflows consume.

### Decoder consumers

The audio decoder factory and decoder sessions consume selected `ao_media` container primitives directly for format dispatch, packet demuxing, and WAVE parsing.
They do not consume `TagFile` metadata candidates.
Decoder lifecycle and PCM behavior belong to the [playback architecture](playback.md) and [decoder session specification](../spec/playback/decoder-session.md).

## Boundaries and dependency direction

- `ao_media` depends on utility primitives and does not depend on tag, library, audio, runtime, or frontends.
- `ao_tag` depends on `ao_media` and utility, but its current public result also depends semantically on `ao_library::TrackBuilder`.
- `ao_library::AudioIdentity` currently depends back on `ao_tag::TagFile`; this is current debt, not an allowed target direction.
- Application runtime may coordinate tag interpretation with library commands, but parser code cannot depend on runtime task or notification types.
- `ao_audio` may reuse `ao_media` container evidence, but media primitives cannot depend on decoder, engine, or playback state.
- Library storage and resource stores never parse external containers independently of the interpretation boundary.
- Frontends and UIModel do not open tag parsers to construct parallel metadata candidates.

## Data and control flow

Library ingestion follows this route:

```text
filesystem path
  -> TagFile extension dispatch and read-only mapping
  -> format reader + shared media primitives
  -> borrowed TrackBuilder candidate + cover spans
  -> runtime library workflow serializes or copies the candidate while TagFile lives
  -> library mutation commits records and resources
  -> revisioned changes reach sources and projections
```

Audio identity follows a separate view of the same mapped file:

```text
TagFile::audioPayload()
  -> borrowed encoded byte range
  -> chunked XXH3-128 accumulation with cooperative cancellation
  -> payload length + signature
  -> scan or identity-index mutation
```

Playback decoding bypasses the metadata candidate:

```text
playback path
  -> decoder factory
  -> decoder session
  -> shared media container primitives where needed
  -> PCM blocks
```

## Structural constraints

- `TagFile` is non-copyable and non-movable because its mapped address and borrowed results are tied to one object lifetime.
- `AudioPayload::bytes` is valid only while the owning `TagFile` remains alive.
- A `TrackBuilder` returned by `loadTrack()` can borrow mapped strings and strings owned by the `TagFile`; it must be serialized or copied before the file is destroyed or `loadTrack()` is called again.
- `loadTrack()` clears the per-file owned-string arena before constructing the next candidate.
- Recognition and open dispatch use one extension table; runtime scanners do not maintain a second supported-file list.
- A payload boundary change alters persisted audio-identity meaning and requires the compatibility action defined by the reference.
- Decoder sessions and metadata interpretation may share container primitives but remain separate behavioral owners.

## Failure, cancellation, and lifetime boundaries

Public media and tag parsing boundaries return `Result` for unsupported input, mapping failures, and malformed external bytes.
Private tag and media exception leaves may shorten local validation paths, but each format boundary catches only the private leaves it can produce and translates them to recoverable results.
Allocation failures and invariant faults are not laundered into corruption errors.

`TagFile::open()` maps before returning the reader, so mapping failure cannot escape as a partially usable public object.
Metadata loading and payload extraction are synchronous and have no cancellation surface.
Chunked audio-identity hashing accepts a stop token; cancellation returns a successful empty optional and never publishes a partial identity.

Callers own the `TagFile` for as long as any returned builder, string view, cover span, or payload span remains in use.
Runtime workflows that need deferred work retain the file beside the borrowed candidate or materialize owned library records before releasing it.

## Implementation map

- [`TagFile`](../../include/ao/tag/TagFile.h), [`TagFile.cpp`](../../lib/tag/TagFile.cpp), and [`Open.cpp`](../../lib/tag/Open.cpp) own mapping, dispatch, interpretation, and payload access.
- [`lib/tag/`](../../lib/tag/) owns FLAC, MP4, MPEG/ID3, and WAVE interpretation.
- [`include/ao/media/`](../../include/ao/media/) and [`lib/media/`](../../lib/media/) own reusable container primitives.
- [`AudioIdentity`](../../include/ao/library/AudioIdentity.h) and [`AudioIdentity.cpp`](../../lib/library/AudioIdentity.cpp) own the current library-side payload hash.
- [`ScanPlanBuilder.cpp`](../../app/runtime/library/ScanPlanBuilder.cpp), [`ScanApplyOperation.cpp`](../../app/runtime/library/ScanApplyOperation.cpp), [`LibraryYamlImporter.cpp`](../../app/runtime/library/LibraryYamlImporter.cpp), and [`LibraryWriter.cpp`](../../app/runtime/library/LibraryWriter.cpp) are principal runtime consumers.
- [`DecoderFactory.cpp`](../../lib/audio/DecoderFactory.cpp), [`WavDecoderSession.cpp`](../../lib/audio/WavDecoderSession.cpp), and [`Mp4PacketSource.cpp`](../../lib/audio/detail/Mp4PacketSource.cpp) are principal decoder consumers of media primitives.

## Test map

- Format tests under [`test/unit/tag/`](../../test/unit/tag/) protect dispatch, mapping, metadata, properties, covers, payloads, and corruption containment.
- Media tests under [`test/unit/media/`](../../test/unit/media/) protect MP4 and WAVE parser primitives.
- [`TagTest.cpp`](../../test/integration/tag/TagTest.cpp) protects interpretation against generated encoded fixtures.
- [`AudioIdentityTest.cpp`](../../test/unit/library/AudioIdentityTest.cpp), [`ScanPlanBuilderTest.cpp`](../../test/unit/runtime/library/ScanPlanBuilderTest.cpp), and [`AudioIdentityIndexerTest.cpp`](../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp) protect identity consumption.
- Decoder tests under [`test/unit/audio/`](../../test/unit/audio/) protect media-primitives-to-PCM integration.

## Related documents

- [Architecture landscape](README.md)
- [System architecture](system-overview.md)
- [Library architecture](library.md)
- [Resource delivery architecture](resource-delivery.md)
- [Playback architecture](playback.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Media file interpretation specification](../spec/media/file-interpretation.md)
- [Supported audio files reference](../reference/media/audio-file.md)
- [Library scan and audio identity specification](../spec/library/runtime/scan-and-identity.md)
- [Decoder session specification](../spec/playback/decoder-session.md)
- [RFC 0020: decoupled media interpretation](../rfc/0020-decoupled-media-interpretation.md)
