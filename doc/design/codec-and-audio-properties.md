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

MP3 bit depth is stored as `0` because MPEG audio does not carry a source PCM bit depth. FLAC and MP4 keep
bit depth when the container exposes it.

AAC is supported for library metadata, YAML, UI, smart-list query data, and playback of MP4/M4A `mp4a`
tracks through FDK-AAC. ADTS `.aac` files are not part of this support. The MP4/M4A decoder factory checks
the sample entry type and creates the ALAC decoder for `alac` or the AAC decoder for `mp4a`, avoiding
extension-only routing.

The AAC decoder outputs interleaved 16-bit PCM and rejects resampling, channel remapping, planar output,
float output, and higher PCM bit-depth requests.

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
