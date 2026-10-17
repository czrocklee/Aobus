---
id: playback.pcm-format
---
# PCM representation and lossless output

## Scope and version

This contract defines logical precision, PCM storage, source channel interpretation, and lossless output admission.
[Audio execution](audio-execution.md) owns negotiation and succession; [decoder sessions](decoder-session.md) own publication and lifetime.
The C++ values have no serialized representation or numeric compatibility promise.

## Code boundary

Public values and Backend operations live under `include/ao/audio/` in the [Core playback layer](README.md).
Decoder interpretation, conversion, lossless candidates, and native mappings live under `lib/audio/`.

## Value surface

### Logical signal

`SignalFormat` describes frame rate, authoritative channel count, meaningful precision, integer/float domain, and an effective WAVE-style speaker mask.
`PcmFormat` describes concrete rate, count, encoding, and mask; it does not duplicate source precision.
A 16-bit signal widened into `Signed32Le` remains a 16-bit source in a 32-bit container.

Identified slots follow ascending assigned WAVE bits; zero leaves positions unidentified.
Count, not mask population, owns interleaved stride.
Formats copy identity without inferring missing roles or permuting samples.
[`SignalFormat.h`](../../../include/ao/audio/SignalFormat.h) owns `kWaveSpeakerMask` and `isCompleteWaveSpeakerMask`: a nonzero, already-normalized set must assign one supported position per channel.
Raw declarations and defaults are interpreted separately by each codec below.
[Channel-order reference](../../reference/media/channel-order.md) owns role sets and codec permutations.

`ChannelLayoutEvidence` records `Identified` interpretation (supported declaration or unambiguous format default), `Inferred` compatibility, or `Unknown`.
Raw declarations and error context stay with their decoder.
Origin affects quality confidence, not concrete mode equality or native admission.

### WAV source interpretation

WAV already uses ascending assigned-bit order; attaching identity does not permute PCM.

| Input | Effective interpretation |
|---|---|
| Plain mono/stereo, absent mask | FC `0x4` / FL-FR `0x3`, `Identified`. |
| Plain 3–8 channels | [WAV compatibility sets](../../reference/media/channel-order.md#canonical-role-sets), `Inferred`. |
| Extensible explicit zero, 1–8 channels | The same compatibility sets, `Inferred`. |
| Complete supported nonzero declaration | Its actual set, `Identified`, including alternate mono/two-channel roles. |
| Excess positional bits | Discard highest positions as WAVE specifies, then validate the effective set. |
| Partial assignment, active reserved positions, wildcard, or absent/zero outside the fallback table | Recoverable source refusal before backend open. Wildcard is never an excess position. |

Complete declarations are not limited to eight channels.
The multichannel and explicit-zero fallbacks are Aobus compatibility policy: WAVE zero normally means direct-out, so these fallbacks can misinterpret discrete/stem content.
The raw parser retains the DWORD independently; playback refusals do not change library import.
See [Microsoft's format reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible) for declaration ordering and excess-bit semantics.

### FLAC source interpretation

FLAC finalizes identity after all metadata, before readiness and output adaptation.
Without a mask comment, [RFC 9639](https://www.rfc-editor.org/rfc/rfc9639.html#section-9.1.3) establishes the exact sets for counts 1, 2, 3, 4, 7 and 8, published as `Identified`.
Its generic five/six-channel back/surround pair does not choose exact WAVE BACK versus SIDE positions: those absent defaults publish zero and `Unknown`.
This underidentifies known front roles rather than declaring non-speaker content; count, precision, storage and decoded slot order remain unchanged.

A complete supported nonzero [mask comment](https://www.rfc-editor.org/rfc/rfc9639.html#section-8.6.2) overrides the default.
The field name and hexadecimal digits are ASCII case-insensitive; syntax requires `0x`/`0X` followed by digits fitting `uint32_t`, accepting leading zero padding.
Whitespace, signs, NULs, trailing bytes and overflow fail rather than becoming absence.
Every declaration must agree, including across multiple Vorbis comment blocks tolerated despite the RFC's single-block rule.
Mask-free blocks have no effect; the first failure owns bounded block/entry diagnostics.

Explicit zero and partial sets are refused because ordinary rendering does not support their unassigned slots.
Excess, reserved and wildcard positions also refuse; FLAC does not apply WAV's truncation rule.
These are playback policies, separate from library import or universal file validity; non-default masks are outside the streamable subset, not inherently invalid.
Identified decoded slots already follow the applicable ascending-bit order.

### ALAC source interpretation

The validated mandatory configuration owns count, rate and precision.
ALAC layout interpretation is not implemented: every channel count, including mono and stereo, publishes zero masks with `Unknown` origin while ordinary decoding remains eligible.
Structural atom/container failures and invalid mandatory configuration remain initialization errors.
Provider element order for counts 3–8 remains unnormalized in the [channel order reference](../../reference/media/channel-order.md#alac); no reference permutation is implemented.

AAC requests FDK WAV output mapping; Opus family 1 permutes decoder mapping.
These byte-order mechanisms do not themselves publish source identity or prove native delivery.
Opus family 255 retains header-defined unassigned output indices, up to 255 channels.

### PCM encoding

`SampleEncoding` defines exact little-endian storage:

| Encoding | Container / nominal precision | Meaning |
|---|---|---|
| `Unknown` | 0 / 0 | No concrete representation. |
| `Signed16Le` | 16 / 16 | Two-byte signed integer. |
| `Signed24PackedLe` | 24 / 24 | Three-byte integer; ALSA `S24_3LE`. |
| `Signed24In32Le` | 32 / 24 | Sign-extended low 24 bits; ALSA `S24_LE`. |
| `Signed32Le` | 32 / 32 | Full-scale signed integer. |
| `Float32Le` | 32 / 32 | IEEE 754 binary32. |

Packed `56 34 12` becomes `56 34 12 00` in `Signed24In32Le`, but `00 56 34 12` when scaled to `Signed32Le`.
Packed negative `FF FF FF` becomes `FF FF FF FF` in `Signed24In32Le`, but `00 FF FF FF` in scaled `Signed32Le`.
Container width and alignment always come from encoding, never logical precision.

### Node formats

`NodeFormat` holds either `SignalFormat` or `PcmFormat`.
Source nodes report logical signals; decoder, engine and stream nodes report concrete bytes when known.
Mixer/server device nodes report their accepted client PCM.
A confirmed direct endpoint reports logical precision: a 24-bit converter behind `S32_LE` is a 24-bit signal, not a 32-bit endpoint.

### Confirmed open outcome

`OpenedPcmMode` contains the canonical client format, optional `ConfirmedEndpoint`, and initial delivery snapshot.
A backend owns native descriptors and any private lossless permutation.
An absent endpoint means unavailable inspection, not a matching endpoint.
Every client and confirmed endpoint must preserve source precision; a confirmed endpoint must also agree in rate, count and domain and fit within the client container.
Its precision bound survives successive tracks spliced during that open.
Unknown or insufficient endpoint precision never permits a lower-precision fallback.

`ChannelDeliveryEvidence` carries independent facts:

- `isMapped`: successful native positioning/configuration established the relationship.
- `nativeSpeakerMask`: observed/configured native positions, possibly partial; zero means unknown. It describes neither canonical native byte order nor physical wiring.
- `hasSharedConversion`: known channel mixing, dropping or relabelling; rate/domain/representation changes remain separate format facts.

The application request cannot manufacture native observations.
Initial evidence is a snapshot; fresher provider observations remain authoritative.
An unresolved known conflict requires a represented conversion or refusal, not erasure to unknown.

### Format interpretation

Concrete-to-logical conversion yields nominal encoding precision, not original source precision.
Both conversion helpers preserve masks.
Mode equality compares rate, count, encoding and mask, excluding interpretation history and logical precision.
[Gapless admission](audio-execution.md#prepared-lookahead) requires exact mode equality plus independent precision/context checks.
The final decoder must retain the inspected effective source format; origin-only drift is accepted.
Encoding names are diagnostic identifiers, not localized copy or persisted identities.

### Lossless output order

The private candidate helper returns the following ordered encodings:

| Signal | Candidates |
|---|---|
| Integer 1–16 bits | `Signed16Le`, `Signed24PackedLe`, `Signed24In32Le`, `Signed32Le`, `Float32Le` |
| Integer 17–24 bits | `Signed24PackedLe`, `Signed24In32Le`, `Signed32Le`, `Float32Le` |
| Integer 25–32 bits | `Signed32Le` |
| 32-bit float | `Float32Le` |

Integer-to-float is bit-transparent only through 24-bit precision.
Zero/unsupported precision and unsupported float widths have no candidates.
Conversion preserves mask, complete frames and the supported sample domain; no silent resampling, channel-count change or lossy fallback is permitted.

### Backend prediction and open

`prewarmFormatHint` is non-binding and performs no native I/O; no hint means no optimistic decoder preparation.
`open` configures a live render target and returns the actual mode, not cached capability or echoed source metadata.
Successful streams have nonzero rate/count and a concrete encoding.
Client/source identities must match; known conflicting direct-endpoint identity refuses, while unknown endpoint positions remain unknown.
A separate conversion stage must represent any downstream rate, count or identity change.

## Implementation and test authority

- [`SignalFormat.h`](../../../include/ao/audio/SignalFormat.h), [`PcmFormat.h`](../../../include/ao/audio/PcmFormat.h), [`SampleEncoding.h`](../../../include/ao/audio/SampleEncoding.h), [`NodeFormat.h`](../../../include/ao/audio/NodeFormat.h), [`ChannelLayoutEvidence.h`](../../../include/ao/audio/ChannelLayoutEvidence.h) and [`ChannelDeliveryEvidence.h`](../../../include/ao/audio/ChannelDeliveryEvidence.h) own values.
- [`DecoderOutput.h`](../../../lib/audio/detail/DecoderOutput.h), [`PcmConversion.cpp`](../../../lib/audio/PcmConversion.cpp) and [`OpenedModeValidation.h`](../../../lib/audio/detail/OpenedModeValidation.h) own candidates, conversion and open-result validation, covered by their [owning tests](../../../test/unit/audio/).
- [`OpenedPcmMode.h`](../../../include/ao/audio/OpenedPcmMode.h) and [`Backend.h`](../../../include/ao/audio/Backend.h) own the open boundary.
- [`WavDecoderChannelLayoutTest.cpp`](../../../test/unit/audio/WavDecoderChannelLayoutTest.cpp), [`FlacDecoderChannelLayoutTest.cpp`](../../../test/unit/audio/FlacDecoderChannelLayoutTest.cpp) cover actual source interpretation and PCM. [`FlacAlsaPipeWireChannelMapTest.cpp`](../../../test/unit/audio/backend/FlacAlsaPipeWireChannelMapTest.cpp) covers decoder-to-planner integration without opening devices.

## Related documents

- [Channel-order reference](../../reference/media/channel-order.md)
- [Audio execution](audio-execution.md)
- [Decoder sessions](decoder-session.md)
- [Quality values](quality-values.md)
