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
- Decoders preserve inspected sample rate, channel count, sample kind, and precision; they do not silently resample, add or remove channels, or reduce precision.
- A codec-native identified speaker layout is normalized to the order owned by the [PCM format reference](../../reference/playback/pcm-format.md); an unidentified channel layout retains its index order.
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

AAC, MP3, and Opus expose a 16-bit decoded integer signal for this contract; their encoded sources remain lossy.
FLAC, ALAC, and integer WAV expose their encoded PCM precision, while float WAV retains the floating-point domain.
Opus is internally a floating-point codec, but decoding it to float would make its sessions unopenable for the integer encodings exclusive-mode devices expose, because the PCM adapter refuses every float-to-integer conversion.

## Stream lifecycle and seeking

The playback path first creates a short-lived inspection decoder with no requested encoding.
Successful inspection supplies complete source and preferred output formats without reading PCM blocks.
Explicit-start preparation next creates, seeks, and prerolls an optimistic final decoder in the selected Backend's valid prewarm hint when one is available.
After a backend selects its exact encoding, playback reuses that prepared decoder on a complete PCM-mode match or creates a fresh ready decoder session with the selected encoding before seeking and preroll.
The backend-selected encoding is always a member of the inspected signal's lossless output set, so the final decoder retains the same precision-preserving contract as optimistic preparation.

Decoder blocks containing PCM remain consumable before a later empty end-of-stream block.
`firstFrameIndex` identifies the actual first PCM frame in a block, including after decoder-level seek adjustment.

Opus decodes at a fixed 48 kHz and reports that rate as its source rate; the
identification packet's input sample rate describes the encoder's source and is
never published.

Every position in an Opus stream is measured against the timeline the container
establishes, which the session derives once and shares with the media-file
reader. A stream cropped at the front or joined from a live source starts at a
nonzero decode origin, and playback starts at that origin advanced past the
header pre-skip, because pre-skip is a length discarded from the decoder output
rather than a region of the absolute timeline. The session discards that
pre-skip before emitting its first frame and stops at the total the timeline
states, so a block sequence carries exactly the audible frames and no
codec-internal padding. A stream whose declared total is zero reaches end of
stream without emitting a block, which is distinct from a stream that never
declared a total and is decoded for as long as it yields packets.

Seeking converts a playback offset into a granule position by measuring from the
playback start, restarts at the earliest packet that can produce a position
80 ms ahead of it, resets decoder state, and discards decoded samples up to the
target, so `firstFrameIndex` names the requested frame exactly. A restart at or
before the first audio packet resumes at the decode origin rather than at zero,
because the header pages carry the granule position zero RFC 7845 fixes for
them, which names no position on a stream that starts cropped. The pre-roll
makes the discarded run long enough for the decoder to converge before the first
audible frame, which matters on streams paged finely enough that a restart would
otherwise land within one packet of the target.

The identification packet's output gain reaches libopus unconverted, which applies the RFC 7845 header gain during decoding.
Channel mapping family 1 orders its channels the way Vorbis does, so the session permutes the mapping it hands libopus into the WAV speaker order every PCM surface expects.
Family 0 is already compatible; family 255 defines no speaker positions and remains in header-defined output-index order.

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
An Ogg stream whose pages carry no complete audio packet is translated to `FormatRejected`, while a malformed page structure or an unusable identification packet preserves its parser error. Because an Ogg page is atomic, a truncation reaching into the first audio page leaves no decodable audio.
An Opus first audio page whose granule position is smaller than the samples it completes is corrupt unless that page also ends the stream, where the smaller position is permitted end trim.
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
