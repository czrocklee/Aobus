---
id: playback.pcm-format
---
# PCM representation and lossless output

## Scope and version

This contract defines how decoders, Engine, quality analysis, and audio backends interpret PCM bytes and logical precision.
Headers own declarations and ordinary defaults; the tables here retain cross-module meanings that declarations alone cannot express.
The types have no serialized or wire-protocol version.
Negotiation behavior belongs to the [audio execution specification](audio-execution.md), and decoder guarantees belong to the [decoder session specification](decoder-session.md).

## Code boundary

This surface belongs to the **Core libraries** layer in the [system architecture](../overview.md), under the [playback architecture](README.md).
Public PCM values and Backend methods live under `include/ao/audio/`; lossless candidate derivation, concrete conversion, and backend mapping live under `lib/audio/`.

## Surface

### Logical signal

`SignalFormat` describes audio independently of its byte container.

Rate counts frames per second, channel count describes interleaved channels, and precision measures meaningful signal bits rather than container width.
The sample kind distinguishes the integer and floating-point domains.

### Channel order

No PCM surface carries channel-layout metadata.
For an identified speaker layout of one through eight channels, the channel count therefore selects this WAV/Microsoft order:

| Channels | Order |
|---:|---|
| 1 | Mono |
| 2 | `FL`, `FR` |
| 3 | `FL`, `FR`, `FC` |
| 4 | `FL`, `FR`, `BL`, `BR` |
| 5 | `FL`, `FR`, `FC`, `BL`, `BR` |
| 6 | `FL`, `FR`, `FC`, `LFE`, `BL`, `BR` |
| 7 | `FL`, `FR`, `FC`, `LFE`, `BC`, `SL`, `SR` |
| 8 | `FL`, `FR`, `FC`, `LFE`, `BL`, `BR`, `SL`, `SR` |

A decoder whose codec orders identified speakers differently normalizes before emitting PCM; it never publishes that codec-native speaker order.
AAC does this through the FDK `AAC_PCM_OUTPUT_CHANNEL_MAPPING` parameter, and Opus mapping family 1 does it by permuting the channel mapping it hands libopus.
FLAC, WAVE, and ALAC are already in this order.

Opus mapping family 255 is the exception to the identified-layout table: it may expose up to 255 channels, but those channels have no assigned speaker positions and remain in header-defined output-index order.

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

### Format interpretation

Converting a concrete PCM format to logical signal facts yields the encoding's nominal precision; it cannot recover the original source precision.
A complete interleaved frame occupies channel count times container bytes per sample; an incomplete format has zero frame bytes.
Concrete mode equality compares rate, channel count, and encoding, not source precision.
Encoding names used for diagnostics are neither persisted identities nor user-facing copy.
The linked headers provide the helper declarations.

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

[Backend.h](../../../include/ao/audio/Backend.h) declares the prediction and open operations.
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
- Decoder session tests under [`test/unit/audio/`](../../../test/unit/audio) lock per-codec production of the selected encoding.

## Related documents

- [Audio execution and concurrency](audio-execution.md)
- [Decoder session](decoder-session.md)
- [Audio quality surface](quality-values.md)
