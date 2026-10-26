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

`SignalFormat` and `PcmFormat` carry an effective WAVE-style `speakerMask`.
A nonzero mask names speakers in ascending assigned-bit order; zero assigns no positions and is never a wildcard or a count-derived default.
Channel count, not mask population, always owns frame geometry.
Source interpretation (`Unknown`, `Identified`, or `Inferred`) is separate from format equality and from native mapping observations.

WAV and FLAC publish effective source identity through their respective [WAV](#wav-source-interpretation) and [FLAC](#flac-source-interpretation) policies below.
The other current codec producers, including ALAC and MP3, still report zero masks and `Unknown` interpretation; byte-order conventions alone do not identify them.
ALSA admits identified sources against its native channel-map observations.
PipeWire admits a complete supported WAVE source declaration and validates the Format param actually read back; the offer is not that observation.
A two-channel offer is one exact role set: the declared WAVE pair when the source mask is nonzero, and FL/FR only when that mask is zero. The offer never writes a new identity into a zero client mask.
Mono and multichannel offers stay unpositioned.
An unpositioned offer does not mean the negotiated observation lacks positions.
Compatible observed constraints may admit transport without proving every declared role.
Only a complete, unambiguous observed WAVE speaker set establishes native mapping evidence, at any channel count and independently of source history.
That evidence does not identify the source or assert canonical native byte order or physical wiring.
The [PipeWire client channel contract](audio-execution.md#pipewire-client-channel-contract) owns readback, partial completion, refresh, and failure classification.
WASAPI's local channel-identity check admits zero or a complete supported WAVE set; an otherwise valid mode with a partial or unsupported set is refused as `TrackRouteIncompatible` / `NotSupported` before native stream access, and prewarm predicts only admissible modes.
The canonical opened client mode retains the source mask.
At every channel count, the Initialize descriptor offers the declared complete role set when the source mask is nonzero; the source and the WAVE descriptor both assign ascending speaker bits, so no byte permutation is needed.
Accepting that descriptor configuration marks the client software boundary as mapped, and is not a claim of endpoint physical wiring, initialized-client readback, or source identification.
Only a zero-mask source keeps the legacy positions: the existing count-derived FC/FL-FR positions for one or two channels, and a zero-mask direct-out native descriptor for higher counts, without proving a speaker mapping.
The WASAPI graph stream publishes this accepted native descriptor, including legacy fills, rather than changing the canonical opened client's source mask.
Native roles can constrain downstream conversion analysis without identifying the source or proving its mapping.
This also applies to independent native-only evidence: ALSA may clear a stream format mask for privately permuted slots while retaining a native speaker set as a downstream constraint.
Core Audio admits complete supported identities through a [bounded AUHAL observation and permutation bridge](audio-execution.md#core-audio-channel-admission).
Its returned client mode stays canonical; native byte order and initial delivery evidence remain separate, and acceptance does not prove physical routing.
Core Audio hints preserve exact source identity without native I/O and are neither admission nor observation guarantees.
The [channel-order reference](../../reference/media/channel-order.md) owns WAV compatibility sets, FLAC's narrower format-default identities, and Opus slot permutations.
Those sets are not a rule for filling a zero PCM or native observation mask.
A decoder publishing identified speakers normalizes any differing codec order before emitting PCM; it never publishes that codec-native speaker order.
Existing AAC and Opus byte-order mechanisms remain unchanged: AAC requests FDK `AAC_PCM_OUTPUT_CHANNEL_MAPPING`, and Opus mapping family 1 permutes the channel mapping it hands libopus.
Neither mechanism itself publishes source identity or proves native delivery.
Opus mapping family 255 may expose up to 255 channels with no assigned speaker positions, in header-defined output-index order.

### WAV source interpretation

WAV already uses ascending assigned-bit order; attaching identity does not permute PCM or change channel-count-based frame stride.
The decoder applies this policy before output adaptation, preserving the effective mask in both source and output formats:

| Input | Effective interpretation |
|---|---|
| Absent mask, mono/stereo | FC `0x4` / FL-FR `0x3`, `Identified` format defaults. |
| Absent mask, 3–8 channels | [WAV compatibility sets](../../reference/media/channel-order.md#canonical-role-sets), `Inferred`. |
| Extensible explicit zero, 1–8 channels | The same compatibility sets, `Inferred`. |
| Complete supported nonzero declaration | Its actual set, `Identified`, including alternate mono/two-channel roles. |
| Excess positional bits | Discard highest set positions until their number matches the channel count, then validate the effective set. |
| Partial assignment, remaining reserved positions, `SPEAKER_ALL`, or absent/zero outside the fallback table | `NotSupported` source refusal before backend open. |

`SPEAKER_ALL` is rejected before trimming; it is never an excess position.
Reserved positions that survive trimming are refused rather than replaced with guessed roles.
Complete supported declarations are not limited to eight channels: nine- and eighteen-channel sets remain valid.
Existing RIFF geometry and precision validation precedes layout interpretation; requested output adaptation still rejects precision loss.
Layout refusal is a recoverable source error, not a native backend observation or an output-fault reclassification.

The multichannel and explicit-zero fallbacks are Aobus compatibility policy: WAVE zero normally means direct-out, so these fallbacks can misinterpret discrete/stem content.
They do not authorize count-derived native identities.
An inferred compatibility mask is nevertheless an effective source identity, not an advisory hint: ALSA applies the same native-map admission and permutation rules as for an explicit identity.
Inference can therefore cause private slot reordering or a route refusal compared with an unpositioned source; missing or compatible partial native observations do not themselves require refusal.
The [raw RIFF format](../../../include/ao/media/wav/Riff.h) retains the declared DWORD independently, distinguishing absence from explicit zero; playback policy does not change library import.
[`WavDecoderSession.cpp`](../../../lib/audio/WavDecoderSession.cpp) owns this interpretation, without changing PCM slot order, seeking, or EOF semantics.
See [Microsoft's format reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible) for declaration ordering and excess-bit semantics.

### FLAC source interpretation

A FLAC session publishes a nonzero effective speaker mask in both source and output formats, with `Identified` interpretation, only when the complete metadata scan establishes an unambiguous identity.
Untrustworthy optional identity evidence instead leaves both masks zero and interpretation `Unknown`, as described below; it never selects an `Inferred` fallback.
The final candidate mask is established once before output adaptation, so source and output identity remain aligned; neither the identity nor PCM is published before required initialization and output admission succeed.
When the metadata scan is trustworthy, every comment block is completely inspectable, and no exact `WAVEFORMATEXTENSIBLE_CHANNEL_MASK` key is present, counts 1, 2, 3, 4, 7, and 8 use the [canonical role sets](../../reference/media/channel-order.md#canonical-role-sets).
These are `Identified` format defaults, not WAV's multichannel `Inferred` compatibility fallback.
Five/six-channel FLAC instead leaves both masks zero and origin `Unknown`: [RFC 9639 §9.1.3](https://www.rfc-editor.org/rfc/rfc9639.html#section-9.1.3) does not resolve its back/surround pair into WAVE BL/BR versus SL/SR.
Changing only the origin to `Inferred` would still publish an effective hard identity, so no count-derived mask is selected for these two counts.
WAV's five/six-channel inferred back masks remain unchanged; the format-specific default policies intentionally differ.
Aobus represents literal mono as FC; seven channels use BC with SL/SR and eight use BL/BR with SL/SR.
These WAVE masks are Aobus's canonical representation, not verbatim RFC mask assignments.
The [reference `flac` 1.5.0 CLI](https://github.com/xiph/flac/blob/1.5.0/src/flac/decode.c) selects side masks `0x607`/`0x60F` for untagged five/six-channel WAVE output, without changing PCM slots; the RFC does not uniquely mandate those or back masks `0x37`/`0x3F`.
Complete supported agreeing declarations of either back or side masks remain `Identified` and retain their distinct roles.
Native delivery evidence cannot independently recover an ambiguously labelled source's original geometric intent or turn its zero mask into source identity.

The standardized comment described in [RFC 9639 §8.6.2](https://www.rfc-editor.org/rfc/rfc9639.html#section-8.6.2) can instead declare any complete set drawn from the eighteen assigned WAVE positions, with exactly one position per source channel.
Its slots follow ascending assigned-bit order, including nondefault sets; the comment does not describe arbitrary channel swaps.
libFLAC already supplies logical decoded slots in that order, so the session adds no PCM permutation for either defaults or explicit identities.

FLAC's [coded channel count](https://www.rfc-editor.org/rfc/rfc9639.html#section-8.2) remains limited to eight; eighteen available role bits do not permit nine or more coded channels.
Unknown identity never admits unsupported coded geometry, so WAV's higher-channel-count policy has no corresponding nine-channel FLAC fallback.

libFLAC retains its default STREAMINFO metadata selection; the session does not request VORBIS_COMMENT callbacks.
Initialization makes one `FLAC__stream_decoder_process_single` call before bulk metadata processing.
Identity inspection is permitted only if that first native step delivered STREAMINFO as the first selected metadata block; libFLAC owns prefix selection, without a copied scanner for its ID3, overlap, or frame-sync rules.
Normal native-tolerated prefixes followed by first-block STREAMINFO remain identifiable.
A separate bounded scan then borrows views from the same file mapping used for decoding, without moving its read cursor, and parses comment fields outside foreign callbacks.
Native STREAMINFO positions and metadata-end bounds cross-check this established origin: the mapped chain starts only where the header at the recorded STREAMINFO offset matches the native delivery. They cannot establish the origin alone.
Native-tolerated metadata with nonfirst STREAMINFO remains `0` / `Unknown`, including when an earlier block holds a valid declaration; a fake marker inside padding never certifies a suffix.
The allocation-free raw [Vorbis comment walker](../../../include/ao/media/VorbisComment.h) is shared with library import, while each consumer retains its own publication and trailing-byte policy.
Playback neither owns each tag nor materializes all comments, and imposes no `100000`-comment cap: entry counts and lengths are bounded by the actual block payload.
This is a parsing/resource contract, not a measured performance improvement.

Only a complete trustworthy metadata chain and completely inspectable comment blocks permit publication of either a declared identity or an absent-comment default:

| Optional identity evidence | Source/output mask and origin |
|---|---|
| Complete trustworthy inspection, no exact recognized key, 1/2/3/4/7/8 channels | Count default, `Identified`. |
| Complete trustworthy inspection, no exact recognized key, 5/6 channels | `0`, `Unknown`; unresolved back/side geometry. |
| Complete supported declaration, all numeric duplicates agree | Exact declared mask, `Identified`. |
| Recognized malformed or conflicting declaration | `0`, `Unknown`. |
| Zero, partial, excess, reserved-bit, or `SPEAKER_ALL` declaration | `0`, `Unknown`; never trimmed or replaced with a default. |
| Nonfirst STREAMINFO, incomplete comments, or inability to inspect optional positions, marker, or native-boundary cross-checks safely | `0`, `Unknown`. |

A short comment payload, out-of-bounds entry length, or unaccounted trailing bytes withholds identity without newly rejecting otherwise-valid STREAMINFO and audio.
Unknown is absorbing for this inspection: neither an earlier valid candidate nor a later good declaration repairs malformed, conflicting, unsupported, or incomplete evidence.
Optional inspection may stop once identity is unknown; it must never resume at a suffix and publish that suffix as complete evidence.
This is not a blanket decode-error recovery rule: native metadata processing, I/O, required STREAMINFO, precision, frame, timeline, and EOF faults retain their failures.
In particular, actual outer metadata truncation rejected by native processing remains a decoding failure, not an optional inspection limitation.
A failed native processing call remains a hard failure even without an error callback.
Raw frames without STREAMINFO retain the required-STREAMINFO diagnostic through either the guarded write callback or an error in native `READ_FRAME` during the initial step before STREAMINFO.
The latter covers implicit rate/precision headers and raw-subframe errors, including frame-phase `LOST_SYNC`, that never reach a write callback; it causes the next read callback to abort, although already buffered native bytes may still be scanned.
This does not override actual native metadata failures such as truncated STREAMINFO or reject tolerated junk/ID3-prefix `LOST_SYNC` notices.
Valid STREAMINFO still supplies implicit code-zero rate/precision values, including ordinary 48000 Hz / 16-bit geometry.
The [initialization failure contract](decoder-session.md#failure-behavior) owns cause precedence and read-abort limits; guarded writes never reach an unconfigured output adapter.

The decoder applies this text and duplicate-resolution policy to readable declarations:

- The complete field name and hexadecimal contents are ASCII case-insensitive.
  Names are not trimmed: leading or trailing spaces name a different, unrecognized field, not a malformed value of the standardized field.
  The value requires `0x` or `0X` followed by at least one hexadecimal digit and must fit numerically in an unsigned 32-bit integer.
  Arbitrary leading zero padding is accepted; there is no eight-digit lexical limit.
- Parsing consumes the full length-delimited value, without whitespace, signs, separators, suffixes, or embedded/trailing NUL bytes.
  A recognized field with missing or malformed contents leaves identity `Unknown`, never treated as an absent comment.
- Repeated declarations identify the source only when every occurrence parses to the same supported numeric value.
  Any malformed recognized occurrence or conflicting value leaves identity `Unknown` regardless of occurrence order.

Whitespace strictness and duplicate resolution are Aobus implementation policy, not requirements specified by the RFC.
Optional channel evidence cannot introduce an extra source refusal: otherwise-valid audio with these declarations may play best-effort as `Unknown`.
Requested-output precision rejection remains a required error and is not hidden by an unknown layout; private output configuration still precedes final STREAMINFO geometry validation.
FLAC never borrows WAV's explicit-zero fallback or excess-bit trimming.
Zero, partial, or nondefault declarations are not necessarily invalid FLAC; a nondefault layout outside the [streamable subset](https://www.rfc-editor.org/rfc/rfc9639.html#section-7) is not thereby invalid FLAC.
Withholding identity is not a general file-validity judgment or a claim that the original speaker intent is safe to ignore.
Lossless WAV-to-FLAC conversion need not preserve identity: a stereo WAV mask `0x7` or reserved-excess `0x40003` is admitted as `0x3` / `Identified` after WAV trimming, whereas those FLAC declarations are admitted as `0` / `Unknown`.
The [reference `flac` 1.5.0 encoder](https://github.com/xiph/flac/blob/1.5.0/src/flac/encode.c) preserves those nonzero excess masks with a warning; warnings-as-errors aborts instead.
That version does not automatically turn a zero WAV mask into an explicit FLAC-zero comment; explicit zero remains distinct from absence.

[`FlacDecoderSession.cpp`](../../../lib/audio/FlacDecoderSession.cpp) owns this playback interpretation.
Sharing the raw comment walker does not change library import's atomic publication or format-specific trailing-byte semantics.
The existing precision-preserving output adaptation, PCM slot order, seek, flush, EOF, and decoding-error contracts remain unchanged.
Replacing the earlier zero-mask producer means a native route can now apply a same-set slot permutation or refuse a known role contradiction instead of treating the source as unpositioned.
For a nonzero source identity and a complete valid current ALSA map, the speaker set must match, not the ordinal order: a same-set private permutation remains available in [`AlsaChannelMap.cpp`](../../../lib/audio/backend/detail/AlsaChannelMap.cpp).
For example, three-channel FLAC's default FL/FR/FC (`0x7`) conflicts with native FL/FR/LFE (`0xB`); seven-channel FLAC's default FL/FR/FC/LFE/BC/SL/SR (`0x70F`) conflicts with FL/FR/FC/LFE/BL/BR/BC (`0x13F`).
Missing native positions still do not manufacture evidence or alone require refusal; known contradictory slots in a partial map can still cause refusal, and source identity does not prove native mapping or physical routing.
Unknown best-effort playback is not a role-safety or native-routing guarantee: zero-mask paths may use ordinal/default transport through different speaker roles, and backend open can still fail.
For example, delivering zero-mask six-channel decoded FL/FR/FC/LFE/pair-left/pair-right unchanged into native slots FL/FR/RL/RR/FC/LFE puts center/LFE bytes in rear-labelled slots and the final pair in center/LFE slots.
These examples describe software labels, not physical speakers, audibility, or device measurements.
Zero alone does not distinguish clean absent-key five/six-channel FLAC from malformed or uninspectable FLAC identity evidence or other unknown codecs; neither zero nor channel count authorizes native role recovery.
Numerical bit-perfectness and channel confidence are independent.
The [lookahead contract](audio-execution.md#prepared-lookahead) discloses same-codec FLAC identity-boundary drain/reopen behavior as well as the existing conditional FLAC/ALAC regression.

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

`PcmFormat` contains `sampleRate`, `channels`, `encoding`, and effective `speakerMask`.
It is the exact byte-level stream mode and deliberately does not duplicate the source's logical precision.
For example, losslessly widened 16-bit audio carried as `Signed32Le` uses the same concrete `PcmFormat` as a full-precision `Signed32Le` stream with the same rate, count, and speaker identity; the track's `SignalFormat` remains the authority that the source contained 16 meaningful bits.

`NodeFormat` is `std::variant<SignalFormat, PcmFormat>`.
A node uses `PcmFormat` when it exchanges concrete bytes and `SignalFormat` when it does not.
Source nodes therefore use `SignalFormat`, and decoder, engine, and stream nodes use `PcmFormat` when their byte representation is known.

Device nodes follow the same rule rather than a fixed choice.
A device fed through a mixer or server graph reports the concrete client format it accepted, so it uses `PcmFormat`.
A direct hardware endpoint reports the precision its converter actually resolves, which is a property of the signal rather than of any container: an `S32_LE` stream in front of a 24-bit converter is a 24-bit endpoint, and copying the client `PcmFormat` onto that node would claim eight bits the hardware discards.
Such a node therefore uses `SignalFormat`.

### Confirmed open outcome

`ConfirmedEndpoint` holds the `SignalFormat` a backend observed behind the mode it configured.
`OpenedPcmMode` pairs canonical render-target `clientFormat` with an optional `ConfirmedEndpoint` and the initial native channel-delivery snapshot projected during that open.
A backend owns any private lossless slot permutation; observed native speaker sets do not assert canonical native byte order.
Current observations can supersede that snapshot; neither value is physical speaker wiring.
PipeWire projection and refresh rules belong to the [client channel contract](audio-execution.md#pipewire-client-channel-contract).

For WASAPI, the opened snapshot describes the configured client boundary, so its `hasSharedConversion` is false.
Private retained evidence also records independently known client-to-engine-mix channel conversion; graph projection attributes that conversion only to the receiving sink, including when no concrete mix format is available.
The client mapping comes from the descriptor whose `Initialize` succeeded; `GetMixFormat` independently observes the shared engine, not the initialized client's input format.
Neither successful configuration nor this observation guarantees acceptance of every source role set on every endpoint or proves physical routing.

The optional WASAPI mix observation keeps independent numerical and role facts separate from a concrete PCM claim:

- Excess highest-position speaker bits are trimmed to the channel count; partial role sets remain useful constraints, but reserved or wildcard bits erase the entire role set.
- Missing rate or count yields no concrete observation and guesses no geometry.
- Contradictory PCM arithmetic withholds concrete format and precision claims while preserving independent rate, count, and usable role facts.
- Successful but short extensible storage is a terminal storage-integrity fault, even when numerical fields are missing; a successful query without a descriptor is also a fault.
- A failed optional query HRESULT supplies no mix evidence, not a reason to reject a separately valid client descriptor.

These observation rules do not relax required native-operation faults or turn them into contextual route incompatibility.
The private [WASAPI format helpers](../../../lib/audio/backend/detail/WasapiFormat.h) own descriptor and observation preconditions.

An absent endpoint means the backend could not inspect a direct endpoint during that open.
It does not mean the endpoint matches the client format.
Endpoint presence never admits a narrower client mode: the client encoding and every confirmed endpoint must preserve the source precision.

### Format interpretation

Converting a concrete PCM format to logical signal facts yields the encoding's nominal precision; it cannot recover the original source precision.
A complete interleaved frame occupies channel count times container bytes per sample; an incomplete format has zero frame bytes.
Concrete mode equality compares rate, channel count, encoding, and effective speaker mask, not source precision or interpretation history.
Logical/concrete format conversion preserves the mask; PCM encoding conversion rejects mismatched logical and storage identity.
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
`canReuseFormat` is a separate control-domain query about retaining that installed transport for a successor's own canonical mode.
It defaults to exact PCM equality; any additional backend proof leaves source identity, cache equality, conversion checks, and endpoint precision validation unchanged.
The [audio execution contract](audio-execution.md#prepared-lookahead) bounds the stereo `0`/`0x3` opt-ins and Core Audio's conservative fallback.

## Validation rules

- Every successful PCM stream has nonzero sample rate and channels plus a non-`Unknown` encoding.
- A selected encoding always preserves the logical signal precision.
- A confirmed endpoint has the configured rate, channels, and sample domain,
  fits inside the client encoding, and preserves the logical source precision.
- An unknown or lower-precision endpoint is never a reason to reduce precision;
  the open is rejected instead.
- Float and integer signals are never converted into each other's domain
  outside the bit-transparent integer-to-float range above.
- The opened client must retain the source rate, count, and effective speaker mask exactly.
  When both source and endpoint masks are nonzero, they must be exactly equal.
  A zero-mask source can use a known endpoint without inheriting its identity; an unknown endpoint mask provides no matching-speaker proof. Zero remains unknown, never a wildcard.
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
- [`WavDecoderChannelLayoutTest.cpp`](../../../test/unit/audio/WavDecoderChannelLayoutTest.cpp) locks WAV fallback origins, declared and trimmed identities, layout refusals, output adaptation, and PCM/seek/EOF preservation.
- [`FlacDecoderChannelLayoutTest.cpp`](../../../test/unit/audio/FlacDecoderChannelLayoutTest.cpp) locks FLAC default and declared identities, exact field-name matching, strict length-bounded value parsing, duplicate resolution, unknown admission for unsupported declarations, output adaptation, and PCM/seek/flush/EOF preservation.
- [`FlacMetadataInitializationTest.cpp`](../../../test/unit/audio/FlacMetadataInitializationTest.cpp) covers required-STREAMINFO initialization diagnostics and valid metadata/prefix controls, independently of optional identity inspection.
- [`FlacWavMaskCompatibilityTest.cpp`](../../../test/unit/audio/FlacWavMaskCompatibilityTest.cpp) locks format-specific WAV/FLAC identity using retained reference-encoder output bytes and independently authored PCM, including trimmed WAV identity versus unknown FLAC for excess masks; it does not run an encoder during the test.
- [`CoreAudioChannelLayoutTest.cpp`](../../../test/unit/audio/backend/detail/CoreAudioChannelLayoutTest.cpp) locks bounded AUHAL observations, canonical/native separation, exact storage permutations, and immutable-plan refresh; [`CoreAudioBackendTest.cpp`](../../../test/unit/audio/backend/CoreAudioBackendTest.cpp) retains native open and lifetime controls.

## Related documents

- [Audio execution and concurrency](audio-execution.md)
- [Decoder session](decoder-session.md)
- [Audio quality surface](quality-values.md)
