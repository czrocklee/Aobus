---
id: playback.decoder-session
type: spec
status: current
domain: playback
summary: Defines decoder session lifecycle, native format preservation, supported PCM representations, seeking, and failure behavior.
---
# Decoder session

## Scope

This specification defines the common `DecoderSession` behavior and the current per-codec PCM output capabilities.
File recognition and library codec values belong to the [supported audio files](../../reference/media/audio-file.md) and [track model](../../reference/library/model/track.md) references.

## Code boundary

This contract belongs to the **core libraries** layer in the [system architecture](../../architecture/system-overview.md) and refines the decode boundary in the [playback architecture](../../architecture/playback.md).
Its public surface lives under `include/ao/audio/` and its implementations live under `lib/audio/`; decoders may consume lower core media and utility facilities but do not depend on application runtime, UIModel, or frontends.

## Invariants

- `DecodedStreamInfo::sourceFormat` describes source stream properties, while `outputFormat` truthfully describes returned PCM.
- A requested zero format field selects the native stream value.
- Output is interleaved; planar output is rejected.
- Decoders preserve native sample rate and channel count and do not silently resample or remap channels.
- Unsupported format requests fail during `open()` before playback begins.
- `close()` is idempotent and clears `streamInfo()`.
- Failed `open()` leaves the session closed and retains no previous stream information.
- Reading an unopened, closed, or exhausted session returns a stable empty end-of-stream block.

## Output representations

| Codec | Supported PCM output |
|---|---|
| AAC | 16-bit integer; or 32-bit integer storage with 16 valid bits. |
| ALAC | Native integer depth; 16-to-32 or 24-to-32 integer padding. |
| FLAC | 16-bit integer, packed 24-bit integer, or 32-bit integer storage. |
| MP3 | 16-bit integer or 32-bit float. |
| WAV | Native integer PCM, supported wider integer storage, or 32-bit float for float input. |

For integer padding, `validBits` records source precision rather than storage width.
A request for different effective precision is rejected unless that decoder implements the conversion.

MP3 source compression has no lossless source bit-depth claim; decoded PCM depth is an output representation.
AAC rejects float output, higher valid-bit requests, resampling, channel remapping, and planar output.

## Stream lifecycle and seeking

The playback path may first open with incomplete output requirements to discover native properties.
Successful `open()` therefore supplies complete source and output formats.

Decoder blocks containing PCM remain consumable before a later empty end-of-stream block.
`firstFrameIndex` identifies the actual first PCM frame in a block, including after decoder-level seek adjustment.

## Codec and gapless observations

`DecodedStreamInfo::codec` and `isLossy` are conservative.
Unknown codecs remain `Unknown`, and every lossy decoder reports lossy.

The current engine splice gate accepts only lossless FLAC, lossless ALAC, and WAV sessions whose negotiated formats are compatible.
Lossy and unknown sessions use the drain path until delay/padding trim is parsed and fixture-tested.

## Failure behavior

Malformed external media and unsupported requests return recoverable decoder errors.
FLAC treats corrupted metadata, headers, frames, CRC mismatches, missing frames, and premature end before the declared sample count as `DecodeFailed`; a standalone loss-of-sync notice may remain recoverable for resynchronization.

Decoder, streaming-source, and factory public entry points use `Result` for external media, IO, and capability failures.
`createDecoderSession` returns a non-null session or a recoverable factory error; `nullptr` is not an alternate error channel.
End of stream is a normal `PcmBlock` value.

Implementations may use the private `ao::audio::detail::DecoderException` and `throwDecoderError` helper to unwind codec helpers.
Each public translating method catches only that leaf and preserves a propagated error's diagnostic location.
Allocation, logic, and invariant faults are not reclassified as decode errors.
The `DecoderSession` surface is `noexcept`, so an unrelated escaping exception fails fast.
The exact operation/code matrix and helper surface are in the [decoder error reference](../../reference/playback/decoder-error.md).

## Implementation map

- [`DecoderSession.h`](../../../include/ao/audio/DecoderSession.h) defines the shared session surface.
- Codec implementations under [`lib/audio/`](../../../lib/audio/) own the format matrix.
- [`DecoderFactory.cpp`](../../../lib/audio/DecoderFactory.cpp) owns container/codec routing.

## Test map

Decoder session and malformed-input tests under [`test/unit/audio/`](../../../test/unit/audio/) lock lifecycle, negotiation, output representation, seeking, and failure behavior.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Supported audio files](../../reference/media/audio-file.md)
- [Track model](../../reference/library/model/track.md)
- [Decoder error reference](../../reference/playback/decoder-error.md)
