---
id: media.audio-file-library-surface
type: reference
status: current
domain: media
summary: Defines the audio file extensions, codec mapping, classical tags, cover roles, and encoded payload ranges used by library import and identity.
---
# Supported audio files

## Scope and version

This reference defines the exact audio-file surface consumed by library scanning, initial metadata import, and audio identity.
Interpretation behavior belongs to the [media file interpretation specification](../../spec/media/file-interpretation.md), while decoder PCM behavior belongs to the [decoder session specification](../../spec/playback/decoder-session.md).

## Code boundary

This surface belongs to the **Core libraries** layer in the [system architecture](../../architecture/system-overview.md), under the [media interpretation architecture](../../architecture/media-interpretation.md).
Recognition, tag parsing, and encoded-payload extraction are public through `include/ao/tag/` and implemented under `lib/tag/`; runtime scan and import consume those results without maintaining a parallel format table.

## Supported extensions and codecs

`TagFile::isSupported()` and `TagFile::open()` share one extension dispatch table.

| Extension | Reader/container | Codec values produced |
|---|---|---|
| `.flac` | FLAC | `Flac` |
| `.mp3` | MPEG audio with ID3 | `Mp3` |
| `.m4a` | MP4 audio | `Alac` for `alac`; `Aac` for `mp4a` |
| `.wav` | RIFF/WAVE | `Wav` |

Extension matching follows the implementation's case normalization.
Other files are skipped by scan rather than becoming error plan items.
ADTS `.aac`, literal `.alac`, `.ogg`, images, and playlists are not supported scan inputs.

MP4 audio currently requires one sample-description entry and sample-to-chunk mappings that reference that first entry.

## Classical tag mapping

| Container | Work | Movement name | Movement number | Movement total |
|---|---|---|---|---|
| FLAC/Vorbis | `WORK` or `GROUPING` | `MOVEMENTNAME` | `MOVEMENT` as `n` or `n/total` | `MOVEMENTTOTAL` |
| MP4/iTunes | `©wrk` or `©grp` text | `©mvn` text | `©mvi` binary big-endian integer | `©mvc` binary big-endian integer |
| ID3v2 | `TIT1` | `MVNM` | `MVIN` as `n/total` | From the total component of `MVIN` |

FLAC/Vorbis maps `CONDUCTOR`, `ENSEMBLE` with `ORCHESTRA` fallback, and `SOLOIST` with `PERFORMER` fallback.
MP4 freeform or metadata keys and ID3v2 `TXXX` keys accept case-insensitive `conductor`, `ensemble`/`orchestra`, and `soloist`; metadata keys also accept the documented work and movement aliases.
Unknown or absent values leave the corresponding track field empty or zero.

## Cover import

FLAC picture blocks and ID3v2 APIC frames preserve source order and normalize numeric picture roles outside `0` through `20` to `Other`.
MP4 cover entries preserve source order and use `FrontCover` because the container entry carries no APIC-style role.

Image bytes are added to the track cover builder and deduplicated later by `ResourceStore`.

## Encoded audio payload

`TagFile::audioPayload()` returns a borrowed non-empty encoded-audio byte span plus its file offset:

| Format | Payload range |
|---|---|
| FLAC | Bytes after the final metadata block. |
| MP4 | Payload of the single top-level `mdat`. |
| MP3 | First MPEG frame after ID3v2/junk through the bytes before trailing ID3v1/APEv2. |
| WAV | RIFF `data` chunk payload. |

The range excludes known tag and non-audio container regions but does not decode samples.
MP3 encoder padding inside MPEG frames remains part of the payload.
Empty or structurally invalid payload regions return a recoverable error.

## Validation rules

Unsupported extensions return `NotSupported` from `TagFile::open`.
Mapping failures return `IoError`; malformed external tag or container bytes return a recoverable format/corruption error according to the [interpretation specification](../../spec/media/file-interpretation.md).
The `TrackBuilder` returned by `loadTrack()` may borrow mapped or reader-owned strings and must be serialized before the `TagFile` is destroyed or loaded again.

## Compatibility and versioning

Adding a supported extension requires adding it to the single open/recognition dispatch and adding parser, scan, payload, and identity tests.
Changing an encoded payload boundary changes persisted identity semantics and therefore requires a library database version increment or an explicitly specified compatible re-index policy.

## Implementation authority

- [`TagFile.h`](../../../include/ao/tag/TagFile.h) defines opening, recognition, metadata load, and payload access.
- Format readers under [`lib/tag/`](../../../lib/tag/) own container-specific mapping and payload boundaries.
- [`AudioCodec.h`](../../../include/ao/AudioCodec.h) owns codec values.

## Test authority

- Format tests under [`test/unit/tag/`](../../../test/unit/tag/) lock metadata, classical fields, covers, codec values, and payload ranges.
- [`ScanPlanBuilderTest.cpp`](../../../test/unit/runtime/library/ScanPlanBuilderTest.cpp) locks scan recognition.
- [`AudioIdentityTest.cpp`](../../../test/unit/library/AudioIdentityTest.cpp) locks payload hashing.

## Related documents

- [Media interpretation architecture](../../architecture/media-interpretation.md)
- [Media file interpretation](../../spec/media/file-interpretation.md)
- [Track model](../library/model/track.md)
- [Library scan and audio identity](../../spec/library/runtime/scan-and-identity.md)
- [Decoder session](../../spec/playback/decoder-session.md)
