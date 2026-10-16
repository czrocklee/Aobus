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
The sample kind distinguishes the integer and floating-point domains. The effective WAVE-style `speakerMask` describes assigned speaker identities independently of the authoritative channel count.

### Channel order

Both shared formats carry an effective `speakerMask`. Identified application slots use ascending assigned WAVE speaker-bit order; zero assigns no speakers and never selects a default from the channel count. Count alone determines the number of interleaved slots and byte stride. These value types do not perform population-count admission, infer missing speakers, or permute samples.

WAV interprets its container declaration before publishing identified canonical PCM; the other codec producers still publish zero masks and unknown source interpretation evidence. Native backends preserve the canonical application identity while owning any private slot permutation. Otherwise admitted routes with genuinely unknown positions remain explicitly legacy-unverified, not verified mappings. Available known contradictions require a correct relationship or contextual refusal; they are not erased to unknown. Null output retains application identities but has no native-delivery evidence.

AAC requests FDK WAV output mapping and Opus family 1 permutes its decoder mapping; these existing mechanisms do not establish end-to-end speaker verification. The current ALAC session does not normalize the provider's codec element order, which cannot be declared canonical from count alone. Its numerical provider and decoding eligibility are unchanged here.

Opus family 255 may expose up to 255 unassigned channels in header-defined output-index order. This slice does not change its decoding eligibility or infer speaker roles.

`ChannelLayoutEvidence` records source interpretation separately from format identity: declared, normative format default, inferred compatibility, or unknown. Its optional decoded, unnormalized WAVE-style declaration distinguishes absence from explicit zero; codec-specific tags, namespaces, and conflict context are not interchangeable with that integer. Origin describes the effective interpretation, not a validity or native-delivery verdict. Evidence participates in stream and route snapshots, but not PCM-mode equality.

### WAV source interpretation

WAV PCM already follows ascending assigned-bit order; assigning identity does not permute its samples. The parser retains the raw DWORD independently of decoder admission, including absence versus an engaged zero.

| Input | Effective identity and evidence |
|---|---|
| Plain mono/stereo, no declaration | FC `0x4` / FL-FR `0x3`, `FormatDefault`. |
| Plain 3–8-channel WAV | The compatibility table below, `Inferred`, with no declaration. |
| Extensible explicit zero, 1–8 channels | The same table, `Inferred`, retaining the engaged raw zero. |
| Complete supported nonzero declaration | Its actual effective set, `Declared`, retaining the original DWORD. Alternate mono/two-channel sets are not relabelled by count. |
| More positional bits than channels | Ignore excess highest bits as WAVE specifies, then validate the effective positions; retain the complete original declaration. |
| Partial assignment, active reserved positions or wildcard | Contextual recoverable source refusal before backend open. A wildcard is not an excess positional bit. |
| Absent/zero declaration outside the table | No invented fallback; ordinary playback refuses the source. Complete supported declarations are not limited to eight channels. |

| Channels | Mask | Canonical roles |
|---:|---:|---|
| 1 | `0x004` | FC |
| 2 | `0x003` | FL FR |
| 3 | `0x007` | FL FR FC |
| 4 | `0x033` | FL FR BL BR |
| 5 | `0x037` | FL FR FC BL BR |
| 6 | `0x03F` | FL FR FC LFE BL BR |
| 7 | `0x70F` | FL FR FC LFE BC SL SR |
| 8 | `0x63F` | FL FR FC LFE BL BR SL SR |

The multichannel table and explicit-zero interpretation are Aobus compatibility policy, not a universal codec or native-device default. WAVE zero normally denotes direct-out; interpreting it as speakers can misinterpret intentional discrete/stem content. Partial, reserved, wildcard and unsupported fallback refusals can remove previously attempted playback eligibility. Import/parser retention is a separate boundary and does not acquire these decoder refusals.
See Microsoft's [WAVEFORMATEXTENSIBLE reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible) for raw ordering, excess-bit and direct-out semantics.

### Existing codec-order limitations

Unknown masks do not repeal existing codec normalization obligations. Opus family 1's canonical destination-to-decoder permutations are `3:[0,2,1]`, `5:[0,2,1,3,4]`, `6:[0,2,1,5,3,4]`, `7:[0,2,1,6,5,3,4]`, and `8:[0,2,1,7,5,6,3,4]`; counts 1, 2 and 4 are identity.

For the ALAC reference element-order convention, the canonical permutations and role sets differ from the WAV fallback table:

| Count | Reference order | Canonical destination-to-source indices | Set |
|---:|---|---|---:|
| 3 | C L R | `1,2,0` | `0x007` |
| 4 | C L R Cs | `1,2,0,3` | `0x107` |
| 5 | C L R Ls Rs | `1,2,0,3,4` | `0x037` |
| 6 | C L R Ls Rs LFE | `1,2,0,5,3,4` | `0x03F` |
| 7 | C L R Ls Rs Cs LFE | `1,2,0,6,3,4,5` | `0x13F` |
| 8 | C Lc Rc L R Ls Rs LFE | `3,4,0,7,5,6,1,2` | `0x0FF` |

This is a reference for the known unnormalized ALAC boundary, not a claim that arbitrary container tags establish that order or that these permutations are implemented by the current ALAC session. ALAC-reference and generic Core Audio/CAF layout-tag namespaces are not interchangeable; equal counts, masks or numeric tags do not prove equal slot order. In particular, reference ALAC four-channel is not quad, seven-channel is not WAV seven-channel, and eight-channel includes wide/back roles. Neither guessing a producer nor a blanket 4/7/8-channel ban repairs this defect.

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

`PcmFormat` contains `sampleRate`, `channels`, `encoding`, and `speakerMask`.
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
`OpenedPcmMode` pairs the canonical render-target `clientFormat` with an optional `ConfirmedEndpoint` and an initial channel-delivery snapshot. Native descriptors and lossless slot permutations are backend-owned; a noncanonical native byte order is not encoded by lying about the application's mask.

An absent endpoint means the backend could not inspect a direct endpoint during that open.
It does not mean the endpoint matches the client format.
Endpoint presence never admits a narrower client mode: the client encoding and every confirmed endpoint must preserve the source precision. The confirmed endpoint remains a precision bound for subsequent spliced sources during that same open; a wider client container does not erase it.

`ChannelDeliveryEvidence` separates a mapped software relationship from legacy uncertainty and from independently known channel conversion. Its native speaker set comes from successful native configuration or observation, not the application request; it does not assert canonical native byte order or physical wiring. Unknown native sets stay zero. Initial-open evidence is a snapshot, never a second authority that can override fresher provider observations. Source origin, native delivery, format-presence verification and aggregate channel confidence are separate facts.

### Format interpretation

Converting a concrete PCM format to logical signal facts yields the encoding's nominal precision; it cannot recover the original source precision.
A complete interleaved frame occupies channel count times container bytes per sample; an incomplete format has zero frame bytes.
Both signal/PCM conversion helpers preserve the effective mask. Concrete mode equality compares rate, channel count, encoding, and speaker mask, not source precision or interpretation history.
An already-installed transport has a separate, narrower reuse proof: [Engine gapless admission](audio-execution.md#prepared-lookahead) may accept an unidentified/canonical-FL-FR stereo transition at identical rate/count/encoding only when that backend proves unchanged execution for both identities. This is not PCM equality, a cache-key alias, a source-mask rewrite or verified speaker evidence. Successor decoder/Engine values keep their actual identity while native configuration remains fixed.
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
`canReusePcmMode` is a separate proof against the installed open, defaulting to exact identity; it does not perform native I/O, negotiate or mutate rendering.
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
- Backends must not substitute sample rate, channel count, or the application speaker mask unless a separate conversion stage is represented in the graph.
- Client/source mask equality is required. A known conflicting nonzero direct-endpoint mask is rejected; an unknown endpoint mask remains unknown, even when endpoint precision is confirmed. No source mask is copied into native readback to fill that gap.
- Encoding conversion checks logical and concrete mask agreement and never relabels channels. A final decoder whose source mask or interpretation evidence changed after inspection is rejected before source preparation.
- PCM blocks contain complete interleaved frames; partial samples and partial frames are invalid.
- No PCM producer, consumer, conversion, or backend mapping may infer concrete
  container width or integer alignment from `SignalFormat::precisionBits`.
  Analysis may compare that field as logical source precision, but byte layout
  always comes from `SampleEncoding`.

## Compatibility and versioning

These values are source-level C++ API with no numeric persistence promise.
Changing enum membership, byte layout, candidate order, field meaning, or helper behavior requires matching decoder, backend, quality, specification, and test updates.

## Implementation authority

- [`SignalFormat.h`](../../../include/ao/audio/SignalFormat.h), [`SampleEncoding.h`](../../../include/ao/audio/SampleEncoding.h), [`PcmFormat.h`](../../../include/ao/audio/PcmFormat.h), and [`NodeFormat.h`](../../../include/ao/audio/NodeFormat.h) own the value surface. [`ChannelLayoutEvidence.h`](../../../include/ao/audio/ChannelLayoutEvidence.h) owns source interpretation; [`ChannelDeliveryEvidence.h`](../../../include/ao/audio/ChannelDeliveryEvidence.h) owns independent native delivery and channel-confidence values.
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
