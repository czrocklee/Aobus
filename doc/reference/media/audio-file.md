---
id: media.audio-file-library-surface
type: reference
status: current
domain: media
summary: Enumerates recognized audio files, visitor fields, source mappings, cover roles, codecs, and encoded payload ranges.
---
# Supported audio files

## Scope and version

This reference defines the exact audio-file surface consumed by library scanning, initial metadata import, and audio identity. Behavior belongs to the [media file reading specification](../../spec/media/file-reading.md); PCM behavior belongs to the [decoder session specification](../../spec/playback/decoder-session.md).

## Code boundary

The public Core surface is `ao::media::file::File`, `Visitor`, and `PayloadView` under `include/ao/media/file/`. Format readers live under `lib/media/file/`; both are part of the `ao_media` target. The [encoded media architecture](../../architecture/encoded-media.md) owns the target boundary and borrowed-data lifetimes, the [system architecture](../../architecture/system-overview.md) owns top-level dependency direction, and application runtime owns the one visitor-to-library adapter described by [library architecture](../../architecture/library.md).

## Supported extensions and codecs

`File::isSupported()` and `File::open()` share one case-normalized extension table.

| Extension | Reader/container | `AudioCodec` emitted |
|---|---|---|
| `.flac` | FLAC | `Flac` |
| `.mp3` | MPEG audio with optional ID3 | `Mp3` |
| `.m4a` | MP4 audio | `Alac` for `alac`; `Aac` for `mp4a`; otherwise no codec callback |
| `.wav` | RIFF/WAVE | `Wav` |

Other paths are unsupported. In particular, ADTS `.aac`, literal `.alac`, `.ogg`, images, and playlists are not recognized scan inputs.

## Visitor surface and order

A successful visit emits present values in this fixed order:

1. `TextField`: `Title`, `Artist`, `Album`, `AlbumArtist`, `Composer`, `Conductor`, `Ensemble`, `Genre`, `Work`, `Movement`, `Soloist`.
2. `NumberField`: `Year`, `TrackNumber`, `TrackTotal`, `DiscNumber`, `DiscTotal`, `MovementNumber`, `MovementTotal`.
3. Technical callbacks: `codec`, `duration`, `bitrate`, `sampleRate`, `channels`, `bitDepth`.
4. `picture` callbacks in source order.

Empty text, zero numeric and technical values, `AudioCodec::Unknown`, and rejected pictures produce no callback. A required failure produces no callback at all.

## Source field mapping

Mappings are case-insensitive for FLAC/Vorbis keys, MP4 metadata/freeform aliases where stated, and ID3 `TXXX` keys. A dash means no mapping.

| Visitor field | FLAC/Vorbis | MP4/iTunes or `mdta` | ID3v2 | WAVE `INFO` |
|---|---|---|---|---|
| `Title` | `TITLE` | `©nam`; `title` | `TIT2` | `INAM` |
| `Artist` | `ARTIST` | `©ART`; `artist` | `TPE1` | `IART` |
| `Album` | `ALBUM` | `©alb`; `album` | `TALB` | `IPRD` |
| `AlbumArtist` | `ALBUMARTIST` | `aART`; `album_artist`, `albumartist` | `TPE2` | — |
| `Composer` | `COMPOSER` | `©wrt`; `composer` | `TCOM` | — |
| `Conductor` | `CONDUCTOR` | freeform/`mdta` `conductor` | `TPE3`; `TXXX:conductor` | — |
| `Ensemble` | `ENSEMBLE`; fallback `ORCHESTRA` | freeform/`mdta` `ensemble`; fallback `orchestra` | `TXXX:ensemble`; fallback `TXXX:orchestra` | — |
| `Genre` | `GENRE` | `©gen`; `genre` | `TCON` | `IGNR` |
| `Work` | `WORK`, `GROUPING` | `©wrk`, `©grp`; `work`, `grouping` | `TIT1`; `TXXX:work`, `TXXX:grouping` | — |
| `Movement` | `MOVEMENTNAME` | `©mvn`; `movementname`, `movement_name`, `mvnm` | `MVNM`; equivalent `TXXX` aliases | — |
| `Soloist` | `SOLOIST`; fallback `PERFORMER` | freeform/`mdta` `soloist` | `TXXX:soloist` | — |
| `Year` | `DATE` | `©day`; `date`, `year` | `TYER`, `TDRC` | first four decimal digits of `ICRD` |
| Track number/total | `TRACKNUMBER` as `n` or `n/total`; `TRACKTOTAL`, `TOTALTRACKS` | `trkn`; `track`, `tracknumber` slash value | `TRCK` slash value | — |
| Disc number/total | `DISCNUMBER` slash value; `DISCTOTAL`, `TOTALDISCS` | `disk`; `disc`, `disk`, `discnumber` slash value | `TPOS` slash value | — |
| Movement number/total | `MOVEMENT` slash value; `MOVEMENTTOTAL` | `©mvi`, `©mvc` signed non-negative big-endian integer; equivalent `mdta` aliases | `MVIN` slash value; equivalent `TXXX` aliases | — |

Unknown fields do not become `TrackBuilder::customMetadata`. Invalid isolated scalar values leave their field at zero. Primary ensemble and soloist mappings win over their fallback aliases.

MP4 integer movement payloads accept widths 1, 2, 3, 4, or 8, data type `0` or `21`, version `0`, and values from `0` through `65535`. Negative, wider, unsupported-type, unsupported-version, and overflowing values are ignored.

## Cover import

`PictureType` uses the ID3v2/FLAC numeric range `0x00` through `0x14` (`Other` through `PublisherLogo`). FLAC picture blocks and ID3v2 APIC frames preserve their source role and order; out-of-range roles normalize to `Other`. MP4 `covr` images preserve source order and emit `FrontCover` because that container entry has no APIC-style role. WAVE `INFO` and embedded ID3 currently contribute no cover callback.

Image bytes are borrowed views at this boundary. `readMediaTrack` passes them to the track cover builder, and `ResourceStore` later owns and deduplicates the persisted bytes.

## Technical properties

| Format | Property sources |
|---|---|
| FLAC | `StreamInfo` sample rate, channels, bit depth, and total-sample duration; bitrate from mapped file bytes and duration. |
| MP3 | First confirmed MPEG frame; table bitrate or adjacent-frame-derived free-format bitrate; Xing frame/byte evidence when valid, otherwise frame bitrate and tag-trimmed payload extent. Bit depth is `16`. |
| MP4 | Selected audio track `mdhd` timing and first `stsd` `alac`/`mp4a` sample entry. Malformed optional timing may leave duration absent. |
| WAVE | Validated format and data chunks; duration from frames and sample rate, bitrate from mapped file bytes and duration. |

## Encoded audio payload

`File::audioPayload()` returns a borrowed non-empty span plus its file offset:

| Format | Payload range |
|---|---|
| FLAC | All bytes after the final metadata block. |
| MP4 | Payload of the single non-empty top-level `mdat`, after its compact or extended header; a size `0` `mdat` extends to end of file. Multiple `mdat` atoms are rejected. |
| MP3 | From the first confirmed MPEG frame through the bytes before validated trailing ID3v1/APEv2. A valid leading ID3v2 envelope is excluded. |
| WAV | RIFF `data` chunk payload. |

The range excludes known tag and non-audio container regions but does not decode samples. MP3 encoder padding inside frames remains part of the payload.

## Errors and lifetime

| Condition | Result |
|---|---|
| Unsupported extension | `NotSupported` from `File::open()` |
| Mapping failure | `IoError` with path context |
| Recognized WAVE with an unsupported format code, extensible subformat, channel count, valid-bit/container combination, PCM bit depth, or float representation | `NotSupported` from `visit()` or `audioPayload()` |
| Required malformed container or empty/ambiguous payload | `CorruptData` or `FormatRejected` |
| Bounded malformed optional metadata subtree | Successful omission |

`File` is move-only and non-concurrent. Payload, text, and image views remain valid while the backing `File` lives; moving the file transfers that backing without copying it. `MediaTrack` is the runtime owner that pairs such views with a library builder.

## Compatibility and versioning

Adding an extension requires the single dispatch entry plus parser, scan, payload, and identity tests. Changing a field mapping requires this reference and matching fixtures to change together. Changing an encoded payload boundary changes persisted identity semantics and requires a database version increment or an explicitly specified compatible re-index policy.

## Implementation authority

- [`File.h`](../../../include/ao/media/file/File.h) and [`Visitor.h`](../../../include/ao/media/file/Visitor.h) define the public surface.
- Format readers under [`lib/media/file/`](../../../lib/media/file/) own source mapping and payload boundaries.
- [`PictureType.h`](../../../include/ao/PictureType.h) owns cover-role values.
- [`AudioCodec.h`](../../../include/ao/AudioCodec.h) owns codec values.

## Test authority

- Format tests under [`test/unit/media/file/`](../../../test/unit/media/file/) lock exact fields, covers, codecs, payloads, errors, and lifetime behavior.
- [`FileTest.cpp`](../../../test/integration/media/file/FileTest.cpp) locks real fixtures.
- [`ScanPlanBuilderTest.cpp`](../../../test/unit/runtime/library/ScanPlanBuilderTest.cpp) locks recognition.
- [`AudioIdentityTest.cpp`](../../../test/unit/library/AudioIdentityTest.cpp) locks payload hashing.

## Related documents

- [System architecture](../../architecture/system-overview.md)
- [Encoded media architecture](../../architecture/encoded-media.md)
- [Library architecture](../../architecture/library.md)
- [Media file reading specification](../../spec/media/file-reading.md)
- [Track model](../library/model/track.md)
- [Library scan and audio identity](../../spec/library/runtime/scan-and-identity.md)
- [Decoder session](../../spec/playback/decoder-session.md)
