# Codec and Hot Audio Properties

## Scope

Aobus stores the audio codec as an application-owned enum, not as a container-specific numeric identifier.
The codec describes the audio encoding format for formats Aobus currently treats as supported library
codecs. It does not describe the file extension or container.

## Storage

`TrackHotHeader` stores:

- `sampleRate` as `uint32_t`
- `codec` as `AudioCodec`
- `bitDepth` as `uint16_t`
- `rating` as `uint8_t`

`sampleRate` is hot because built-in smart lists and common technical filters can use it without loading
cold data. `TrackHotHeader` is 40 bytes and `TrackColdHeader` is 32 bytes, keeping the fixed per-track
header total at 72 bytes.

## Codec Mapping

`AudioCodec` is the canonical storage value:

- `Unknown`
- `Flac`
- `Alac`
- `Aac`
- `Mp3`

The raw storage values are explicit: `Unknown = 0`, `Flac = 1`, `Alac = 2`, `Aac = 128`, and `Mp3 = 129`.
The gap keeps lossless codecs grouped before lossy codecs without making expression evaluation depend on
enum ordering.

The shared helpers are:

- `audioCodecName(AudioCodec)` for UI and YAML export
- `parseAudioCodecName(string_view)` for YAML import and query compilation
- `audioCodecFromStorage(uint8_t)` for defensive conversion from raw storage

Unknown raw values are normalized to `AudioCodec::Unknown`.

## Tag Parsing

Tag readers set codec directly:

- MPEG audio sets `AudioCodec::Mp3`
- FLAC sets `AudioCodec::Flac`
- MP4 `alac` sample entries set `AudioCodec::Alac`
- MP4 `mp4a` sample entries set `AudioCodec::Aac`

MP3 bit depth is stored as `16`, matching the decoder's current PCM output path. FLAC and MP4 keep bit
depth when the container exposes it.

AAC is supported for library metadata, YAML, UI, smart-list query data, and playback of MP4/M4A `mp4a`
tracks through FDK-AAC. ADTS `.aac` files are not part of this support. The MP4/M4A decoder factory checks
the sample entry type and creates the ALAC decoder for `alac` or the AAC decoder for `mp4a`, avoiding
extension-only routing. When an MP4 has non-audio tracks before the audio track, sample-entry detection,
tag parsing, and demuxing select the matching audio `trak` and read its `mdhd`, `stsd`, and sample table
together.

Aobus currently supports MP4 audio tracks with a single `stsd` sample-description entry. The demuxer
rejects tracks with multiple sample descriptions or `stsc` mappings that reference anything other than
that first entry, instead of silently binding packets to the wrong decoder configuration.

The AAC decoder outputs interleaved 16-bit PCM by default. It also accepts a 32-bit PCM container request
when `validBits` remains `16`, padding the decoded 16-bit samples into 32-bit slots. It rejects resampling,
channel remapping, planar output, float output, and higher valid-bit requests.

## Decoder Output Contract

All decoder sessions use the same lifecycle and fixed-output rules:

- `Format` fields set to zero select the native stream value.
- Decoder output is interleaved PCM; planar output is rejected.
- Resampling and channel remapping are rejected.
- Unsupported sample formats are rejected by `open()`, before playback starts.
- `close()` is idempotent and clears `streamInfo()`.
- A failed `open()` leaves the session closed without retaining information from a previously opened file.
- Reading an unopened, closed, or exhausted session returns a stable empty end-of-stream block.
- FLAC reports corrupted metadata, headers, frames, CRC mismatches, missing frames, and an end of stream
  before the declared sample count as `DecodeFailed`. A standalone loss-of-sync notification remains
  recoverable so libFLAC can resynchronize after operations such as `flush()`.

The supported decoder output formats are:

| Codec | Output formats |
| --- | --- |
| AAC | 16-bit integer, or 32-bit integer storage with 16 valid bits |
| ALAC | Native integer depth, 16-bit to 32-bit padding, or 24-bit to 32-bit padding |
| FLAC | 16-bit integer, packed 24-bit integer, or 32-bit integer storage |
| MP3 | 16-bit integer or 32-bit float |

For integer padding, `validBits` records the source precision rather than the storage width. Requests for
a different effective precision are rejected unless the decoder implements that conversion explicitly.

AAC and ALAC share a private MP4 packet-source component for file mapping, track selection, packet
iteration, timing, and seeking. FLAC and MP3 share a private mapped-file cursor used by their library
callbacks. Codec initialization, compressed-data decoding, and PCM conversion remain codec-specific.

## Query

Smart list expressions use the user-facing field:

```text
@codec = FLAC
@codec != MP3
@sampleRate >= 96000 and @bitDepth >= 24
```

`@codec` constants are parsed at compile time into the `AudioCodec` storage value, so evaluation is an
integer comparison. `@sampleRate`, `@codec`, and `@bitDepth` are hot-only fields.

`@lossless` is intentionally not part of this change. A lossless list can be expressed as:

```text
@codec = FLAC or @codec = ALAC
```

## YAML and UI

YAML exports codec as a string:

```yaml
codec: FLAC
sampleRate: 96000
bitDepth: 24
```

YAML import accepts the codec string case-insensitively. Unknown codec strings leave the track codec as
`Unknown`. UI formatting uses the same codec name helper and displays no codec text for `Unknown`.
