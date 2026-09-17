---
id: architecture.encoded-media
---
# Encoded media architecture

## Find the contract for your change

- [Recognition, parsing, and mapped-view lifetime](file-reading.md)
- [Supported files and imported metadata](../../reference/media/audio-file.md)
- [Verified cover delivery](../resource/README.md)
- [Decoder sessions and PCM behavior](../playback/decoder-session.md)

## Scope

Encoded media is the Core boundary between encoded file bytes and the library-ingestion, audio-identity, and decoder consumers that use them.
This page explains that boundary, its dependency direction, and its borrowed-data lifetime model.
Exact extensions, metadata mappings, cover roles, payload ranges, and malformed-input results belong to the linked file-reading specification and audio-file reference; decoder output belongs to playback.

## Boundary and consumers

The `ao_media` target has two cooperating surfaces:

```text
encoded filesystem bytes
          |
          v
ao_media
  |-- media::file::File -> Visitor / PayloadView
  |              |              |
  |              |              `-> runtime audio-identity workflow
  |              `-> runtime readMediaTrack -> library TrackBuilder
  |
  `-- MP4 / Ogg / WAVE container primitives
                 |                         |
                 +-> media file readers    `-> ao_audio decoders -> PCM
```

`media::file::File` owns a read-only mapping, selects a format reader, and exposes normalized visitor evidence plus the encoded payload used for audio identity.
Format readers do not know about library records, runtime commands, UI models, or frontends.

Reusable MP4, Ogg, Opus-timeline, and WAVE primitives expose validated byte structure to both file readers and decoders.
They remain representation mechanisms: they do not construct tracks, choose playback policy, or publish user-facing failures.
FLAC traversal remains private to its file reader because it has no second structural consumer; FLAC decoding remains in the audio decoder and its codec library.

Application runtime is the composition layer allowed to know both media and library types.
`ao::rt::readMediaTrack` adapts one visitor into a `TrackBuilder`, while audio identity separately passes a borrowed payload span to the library hash operation.
Core audio decoders own their own mappings and session state and may reuse container primitives without consuming the library-oriented visitor.
Recognition for ingestion and decoder selection are therefore independent capabilities.

## Dependency direction

- `ao_media` depends on `ao_utility`, not on audio, library, runtime, UIModel, or frontend targets.
- `ao_audio` may depend on `ao_media`; the reverse dependency is forbidden.
- `ao_library` hashes caller-owned payload bytes and does not open or parse media files.
- Runtime may compose `ao_media` and `ao_library`; UIModel and frontends do not parse containers or retain mapped-media views.
- Container primitives expose validated structure and borrowed ranges, not product policy.

The `ao_media_boundary_check` and `ao_library_media_file_boundary_check` targets protect the principal forbidden directions.
The [system architecture](../overview.md) owns the top-level layer model.

## Ownership and lifetime

A `media::file::File` is move-only, sequential, and non-concurrent; const operations may populate lazy caches.
Visitor strings, picture bytes, and payload bytes borrow storage from the mapping or the file's interpreted-content cache.
Moving the `File` transfers that backing; destroying it invalidates every outstanding view.

`MediaTrack` retains the backing `File` for its builder and declares the file before the builder so reverse member destruction releases the borrower first.
It is move-constructible but not move-assignable: assignment could replace the backing before releasing borrowed builder views.
A builder copied or moved out of a `MediaTrack` must not outlive that `MediaTrack` while it still contains media-derived views.

Container views similarly cannot outlive their source bytes.
Decoder mappings and packet views instead belong to each decoder session and retire with that session.
These ownership models are independent even where the two paths share a parser primitive.

## Data and control flow

### Library ingestion

```text
path -> File::open -> required index and visitor evidence
     -> readMediaTrack -> MediaTrack-backed TrackBuilder
     -> library workflow preparation and mutation
```

Reading is synchronous and starts no library transaction or runtime publication.
The consuming library workflow owns transaction timing, reconciliation, cancellation, and change events.
Required file evidence is prepared before callbacks begin, so a required read failure cannot partially mutate the visitor adapter.

### Audio identity

```text
path -> File::open -> borrowed PayloadView
     -> runtime progress and stop token
     -> library::readAudioIdentity(bytes)
     -> library comparison or backfill policy
```

The media boundary owns the payload range; the library owns the hash and its persistence meaning.
Changing a payload boundary therefore requires an explicit identity compatibility or re-index decision.

### Decoding

```text
playback input -> ao_audio decoder/session
               -> ao_media container primitives where useful
               -> encoded packets or samples -> PCM
```

Sharing a primitive does not share a mapping, cache, cancellation source, or lifetime between ingestion and playback.

## Failure and cancellation boundary

Malformed external bytes and I/O failures are recoverable results at public media and decoder boundaries; invariant failures remain programmer faults.
`media::file` performs synchronous caller-thread work and accepts no cancellation token.
Runtime adds cancellation around longer operations such as payload hashing, while decoder sessions own decode and seek cancellation.
Cancellation never turns borrowed evidence into owned data or extends its lifetime.

The [media file reading specification](file-reading.md) owns required-versus-optional parse behavior and observable error classes.
The [outcome channel specification](../failure/outcome-channel.md) owns common result-channel behavior.

## Source boundaries

- [`include/ao/media/file/`](../../../include/ao/media/file) and [`lib/media/file/`](../../../lib/media/file) define the encoded-file boundary and format readers.
- [`include/ao/media/`](../../../include/ao/media) and [`lib/media/`](../../../lib/media) contain reusable container primitives.
- [`MediaTrack.h`](../../../app/runtime/library/MediaTrack.h) owns runtime visitor adaptation and backing lifetime.
- [`lib/media/CMakeLists.txt`](../../../lib/media/CMakeLists.txt) defines `ao_media` and its dependency checks.

Detailed implementation and test authorities are listed by the behavior pages above rather than duplicated here.

## Related documents

- [Architecture landscape](../README.md)
- [System architecture](../overview.md)
- [Library architecture](../library/structure.md)
- [Playback architecture](../playback/README.md)
- [Resource delivery architecture](../resource/README.md)
- [Media file reading specification](file-reading.md)
- [Supported audio files reference](../../reference/media/audio-file.md)
- [Library scan and audio identity](../library/scan-and-identity.md)
