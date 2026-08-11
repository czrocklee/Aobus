---
id: playback.pcm-format
type: reference
status: current
domain: playback
summary: Enumerates logical audio signal fields, concrete PCM encodings, byte layout, and lossless output candidates.
---
# PCM format surface

## Scope and version

This reference enumerates the in-process C++ format vocabulary used between decoders, Engine, quality analysis, and audio backends.
The types have no serialized or wire-protocol version.
Negotiation behavior belongs to the [audio execution specification](../../spec/playback/audio-execution.md), and decoder guarantees belong to the [decoder session specification](../../spec/playback/decoder-session.md).

## Code boundary

This surface belongs to the **Core libraries** layer in the [system architecture](../../architecture/system-overview.md), under the [playback architecture](../../architecture/playback.md).
Public PCM values and Backend methods live under `include/ao/audio/`; lossless candidate derivation, concrete conversion, and backend mapping live under `lib/audio/`.

## Surface

### Logical signal

`SignalFormat` describes audio independently of its byte container.

| Field | Type | Default | Meaning |
|---|---|---:|---|
| `sampleRate` | `std::uint32_t` | `0` | Frames per second. |
| `channels` | `std::uint8_t` | `0` | Interleaved channel count. |
| `precisionBits` | `std::uint8_t` | `0` | Meaningful signal precision. |
| `sampleKind` | `SampleKind` | `Integer` | Integer or floating-point signal domain. |

`SampleKind` contains `Integer` and `FloatingPoint`.

### PCM encoding

`SampleEncoding` identifies the exact little-endian bytes exchanged with a backend.

| Enumerator | Container | Nominal precision | Byte/layout meaning |
|---|---:|---:|---|
| `Unknown` | 0 | 0 | No concrete encoding. |
| `Signed16Le` | 16 bits | 16 bits | Two-byte signed integer. |
| `Signed24PackedLe` | 24 bits | 24 bits | Three-byte signed integer; ALSA `S24_3LE`. |
| `Signed24In32Le` | 32 bits | 24 bits | Four-byte signed word containing a sign-extended value in its low 24 bits; ALSA `S24_LE`. |
| `Signed32Le` | 32 bits | 32 bits | Four-byte signed integer using the full 32-bit scale; ALSA `S32_LE`. |
| `Float32Le` | 32 bits | 32 bits | Four-byte IEEE 754 binary32 sample. |

`Signed24In32Le` and `Signed32Le` are not interchangeable.
For example, packed bytes `56 34 12` become `56 34 12 00` in `Signed24In32Le`, but become `00 56 34 12` when scaled to `Signed32Le`.
The negative packed value `FF FF FF` becomes `FF FF FF FF` in `Signed24In32Le`.

### Concrete PCM format

`PcmFormat` contains `sampleRate`, `channels`, and `encoding`.
It is the exact byte-level stream mode and deliberately does not duplicate the source's logical precision.
For example, losslessly widened 16-bit audio carried as `Signed32Le` uses the same concrete `PcmFormat` as any other `Signed32Le` stream; the track's `SignalFormat` remains the authority that the source contained 16 meaningful bits.

`NodeFormat` is `std::variant<SignalFormat, PcmFormat>`.
A node uses `PcmFormat` when it exchanges concrete bytes and `SignalFormat` when it does not.
Source nodes therefore use `SignalFormat`, and decoder, engine, and stream nodes use `PcmFormat` when their byte representation is known.

Device nodes follow the same rule rather than a fixed choice.
A device fed through a mixer or server graph reports the concrete client format it accepted, so it uses `PcmFormat`.
A direct hardware endpoint reports the precision its converter actually resolves, which is a property of the signal rather than of any container: an `S32_LE` stream in front of a 24-bit converter is a 24-bit endpoint, and copying the client `PcmFormat` onto that node would claim eight bits the hardware discards.
Such a node therefore uses `SignalFormat`.

### Confirmed open outcome

`ConfirmedEndpoint` holds the `SignalFormat` a backend observed behind the mode it configured.
`OpenedPcmMode` pairs the `clientFormat` handed to the native API with an optional `ConfirmedEndpoint`.

An absent endpoint means the backend could not inspect a direct endpoint during that open.
It does not mean the endpoint matches the client format.
Endpoint presence never admits a narrower client mode: the client encoding and every confirmed endpoint must preserve the source precision.

### Helpers

| Helper | Result |
|---|---|
| `encodingNominalBits` | Nominal integer precision or float width owned by an encoding. |
| `encodingContainerBits` | Physical container width owned by an encoding. |
| `bytesPerSample` | Container bytes for one encoded sample. |
| `frameBytes` | `channels * bytesPerSample(encoding)`, or zero for an incomplete format. |
| `signalFormat(PcmFormat)` | Rate, channels, nominal precision, and domain implied by the concrete encoding. It does not recover source precision. |
| `pcmFormat(SignalFormat, SampleEncoding)` | Concrete PCM value using the signal's rate/channels and the supplied encoding. |
| `samePcmMode` | Equality of sample rate, channels, and encoding. |
| `sampleEncodingName` | Stable diagnostic name for an encoding; not persisted or user-facing. |

### Lossless output order

The private lossless-output helper returns only encodings that can retain every bit of the inspected signal, in this order:

| Signal | Ordered encodings |
|---|---|
| Integer, 1–16 bits | `Signed16Le`, `Signed24PackedLe`, `Signed24In32Le`, `Signed32Le`, `Float32Le` |
| Integer, 17–24 bits | `Signed24PackedLe`, `Signed24In32Le`, `Signed32Le`, `Float32Le` |
| Integer, 25–32 bits | `Signed32Le` |
| 32-bit float | `Float32Le` |

Integer-to-float conversion is considered bit-transparent only through 24-bit integer precision.
Zero precision, integer precision above 32 bits, and unsupported float precision produce no candidates.

### Backend prediction and open surface

The Backend contract is:

```cpp
std::optional<PcmFormat> Backend::prewarmFormatHint(
  SignalFormat const& sourceFormat) const noexcept;

Result<OpenedPcmMode> Backend::open(
  SignalFormat const& sourceFormat,
  RenderTarget& target);
```

`prewarmFormatHint` returns a non-binding prediction derived without native I/O; an empty result means no optimistic decoder output is prepared.
`open` requires a live render target and returns the mode actually configured for it, not a cached device capability or an echo of a requested container.

## Validation rules

- Every successful PCM stream has nonzero sample rate and channels plus a non-`Unknown` encoding.
- A selected encoding always preserves the logical signal precision.
- A confirmed endpoint has the configured rate, channels, and sample domain,
  fits inside the client encoding, and preserves the logical source precision.
- An unknown or lower-precision endpoint is never a reason to reduce precision;
  the open is rejected instead.
- Float and integer signals are never converted into each other's domain
  outside the bit-transparent integer-to-float range above.
- Backends must not substitute sample rate or channel count unless a separate conversion stage is represented in the graph.
- PCM blocks contain complete interleaved frames; partial samples and partial frames are invalid.
- No PCM producer, consumer, conversion, or backend mapping may infer concrete
  container width or integer alignment from `SignalFormat::precisionBits`.
  Analysis may compare that field as logical source precision, but byte layout
  always comes from `SampleEncoding`.

## Compatibility and versioning

These values are source-level C++ API with no numeric persistence promise.
Changing enum membership, byte layout, candidate order, field meaning, or helper behavior requires matching decoder, backend, quality, specification, and test updates.

## Implementation authority

- [`SignalFormat.h`](../../../include/ao/audio/SignalFormat.h), [`SampleEncoding.h`](../../../include/ao/audio/SampleEncoding.h), [`PcmFormat.h`](../../../include/ao/audio/PcmFormat.h), and [`NodeFormat.h`](../../../include/ao/audio/NodeFormat.h) own the value surface.
- The private [`DecoderOutput.h`](../../../lib/audio/detail/DecoderOutput.h) owns lossless candidate derivation.
- [`PcmConversion.cpp`](../../../lib/audio/PcmConversion.cpp) owns byte conversion between concrete encodings.
- The private [`OpenedModeValidation.h`](../../../lib/audio/detail/OpenedModeValidation.h) owns conversion admissibility and open-result validation.
- [`OpenedPcmMode.h`](../../../include/ao/audio/OpenedPcmMode.h) and [`Backend.h`](../../../include/ao/audio/Backend.h) own the open outcome and signature.

## Test authority

- [`DecoderOutputTest.cpp`](../../../test/unit/audio/DecoderOutputTest.cpp) locks private candidate membership, order, and preferred selection.
- [`PcmConversionTest.cpp`](../../../test/unit/audio/PcmConversionTest.cpp) locks packed, 24-in-32, full-scale 32-bit, float, and precision-loss rejection behavior.
- [`OpenedModeValidationTest.cpp`](../../../test/unit/audio/OpenedModeValidationTest.cpp) locks strict lossless client and endpoint validation.
- Decoder session tests under [`test/unit/audio/`](../../../test/unit/audio/) lock per-codec production of the selected encoding.

## Related documents

- [Audio execution and concurrency](../../spec/playback/audio-execution.md)
- [Decoder session](../../spec/playback/decoder-session.md)
- [Audio quality surface](quality-surface.md)
