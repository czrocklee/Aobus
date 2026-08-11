---
id: playback.decoder-session
type: spec
status: current
domain: playback
summary: Defines ready decoder-session behavior, native format preservation, supported PCM representations, seeking, and failure behavior.
---
# Decoder session

## Scope

This specification defines the common `DecoderSession` behavior and the current per-codec PCM output capabilities.
File recognition and library codec values belong to the [supported audio files](../../reference/media/audio-file.md) and [track model](../../reference/library/model/track.md) references.

## Code boundary

This contract belongs to the **core libraries** layer in the [system architecture](../../architecture/system-overview.md) and refines the decode boundary in the [playback architecture](../../architecture/playback.md).
Its public surface lives under `include/ao/audio/` and its implementations live under `lib/audio/`; decoders may consume lower core media and utility facilities but do not depend on application runtime, UIModel, or frontends.
Each source-private concrete decoder is created only through its static `open(path, outputEncoding)` factory.
Its constructor and one-shot `initialize()` step are private, and it exposes no instance `open()`, `close()`, or reopen lifecycle.

## Invariants

- `DecodedStreamInfo::sourceFormat` describes source stream properties, while `outputFormat` truthfully describes returned PCM.
- Every published `DecoderSession` is already open and has complete, valid `streamInfo()`; the public interface has no unopened or closed state.
- A decoder created without a requested encoding inspects the stream and chooses its preferred lossless native representation.
- A decoder created with a requested `SampleEncoding` either produces exactly that interleaved representation or is not published.
- Decoders preserve inspected sample rate, channel count, sample kind, and precision; they do not silently resample, remap channels, or reduce precision.
- Unsupported or precision-losing encoding requests fail during source-private construction before playback begins.
- An exhausted session returns a stable empty end-of-stream block; destruction ends the session and releases its codec resources.

## Output representations

Every current decoder normalizes its codec-native output through the same precision-preserving PCM adapter.
The resulting set depends on the inspected signal rather than the output device:

| Inspected signal | Supported output encodings |
|---|---|
| Integer, at most 16 bits | Signed 16-bit; packed 24-bit; 24-bit in 32-bit; signed 32-bit; 32-bit float. |
| Integer, 17–24 bits | Packed 24-bit; 24-bit in 32-bit; signed 32-bit; 32-bit float. |
| Integer, 25–32 bits | Signed 32-bit. |
| 32-bit float | 32-bit float. |

The ordered exact enum surface and byte layouts belong to the [PCM format reference](../../reference/playback/pcm-format.md).
Integer-to-float mapping is bit-transparent only through 24-bit precision.
No decoder advertises or accepts a representation that reduces the inspected signal precision.

AAC and MP3 expose a 16-bit decoded integer signal for this contract; their encoded sources remain lossy.
FLAC, ALAC, and integer WAV expose their encoded PCM precision, while float WAV retains the floating-point domain.

## Stream lifecycle and seeking

The playback path first creates a short-lived inspection decoder with no requested encoding.
Successful inspection supplies complete source and preferred output formats without reading PCM blocks.
Explicit-start preparation next creates, seeks, and prerolls an optimistic final decoder in the selected Backend's valid prewarm hint when one is available.
After a backend selects its exact encoding, playback reuses that prepared decoder on a complete PCM-mode match or creates a fresh ready decoder session with the selected encoding before seeking and preroll.
The backend-selected encoding is always a member of the inspected signal's lossless output set, so the final decoder retains the same precision-preserving contract as optimistic preparation.

Decoder blocks containing PCM remain consumable before a later empty end-of-stream block.
`firstFrameIndex` identifies the actual first PCM frame in a block, including after decoder-level seek adjustment.

MP3 construction scans the complete stream with mpg123 before reporting stream
information. The scan builds an exact frame index and duration even when the
stream has no Xing or VBRI seek table. Seek rejects negative offsets and offsets
beyond a known exact duration; sequential decode and stable EOF behavior remain
unchanged. The scan performs file-size-linear work during decoder preparation.

## Codec and gapless observations

`DecodedStreamInfo::codec` and `isLossy` are conservative.
Unknown codecs remain `Unknown`, and every lossy decoder reports lossy.

The current engine splice gate accepts only lossless FLAC, lossless ALAC, and WAV sessions whose concrete PCM modes are compatible.
The already-open mode must preserve the successor signal; otherwise the transition drains and reopens.
Lossy and unknown sessions use the drain path until delay/padding trim is parsed and fixture-tested.

## Failure behavior

Malformed external media and unsupported requests return recoverable decoder errors.
FLAC treats corrupted metadata, headers, frames, CRC mismatches, missing frames, and premature end before the declared sample count as `DecodeFailed`; a standalone loss-of-sync notice may remain recoverable for resynchronization.

Decoder operations and streaming-source entry points use `Result` for external media, IO, and capability failures.
The source-private `openDecoderSession` construction boundary returns a non-null ready session or a recoverable error; `nullptr` is not an alternate error channel.
MP4 factory routing stops after selecting the first usable audio track. No matching audio track is translated to `NotSupported`, while a structural parser failure encountered before selection preserves its parser error.
WAV construction validates chunk boundaries only through the first complete supported `fmt` and non-empty `data` pair; unrelated later chunks do not prevent decoding already-bounded audio data.
End of stream is a normal `PcmBlock` value.

Implementations may use the private `ao::audio::detail::DecoderException` and `throwDecoderError` helper to unwind codec helpers.
Each public translating method catches only that leaf and preserves a propagated error's diagnostic location.
Allocation, logic, and invariant faults are not reclassified as decode errors.
The `DecoderSession` surface is `noexcept`, so an unrelated escaping exception fails fast.
The exact operation/code matrix and helper surface are in the [decoder error reference](../../reference/playback/decoder-error.md).

## Implementation map

- [`DecoderSession.h`](../../../include/ao/audio/DecoderSession.h) defines the shared ready-session surface.
- [`DecoderOutputAdapter`](../../../lib/audio/detail/DecoderOutputAdapter.h) and codec implementations under [`lib/audio/`](../../../lib/audio/) own the output matrix.
- [`DecoderSessionBase.h`](../../../lib/audio/detail/DecoderSessionBase.h) and the concrete codec implementations own factory-only concrete construction, private initialization, and destruction of failed candidates.
- [`DecoderFactory.h`](../../../lib/audio/DecoderFactory.h) and [`DecoderFactory.cpp`](../../../lib/audio/DecoderFactory.cpp) own extension and MP4 sample-entry routing plus successful concrete-to-`DecoderSession` conversion.

## Test map

Decoder session and malformed-input tests under [`test/unit/audio/`](../../../test/unit/audio/) lock ready construction, requested-output adaptation, output representation, seeking, and failure behavior.
[`DecoderErrorTest.cpp`](../../../test/unit/audio/DecoderErrorTest.cpp) mechanically rejects direct concrete construction and instance `close()` operations.
[`DecoderFactoryTest.cpp`](../../../test/unit/audio/DecoderFactoryTest.cpp) locks ready construction, routing, and initialization failure.
[`DecoderOutputAdapterTest.cpp`](../../../test/unit/audio/detail/DecoderOutputAdapterTest.cpp) directly locks rejection of precision loss, pass-through behavior, exact byte conversion, and reset.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Supported audio files](../../reference/media/audio-file.md)
- [Track model](../../reference/library/model/track.md)
- [Decoder error reference](../../reference/playback/decoder-error.md)
