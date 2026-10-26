---
id: media.channel-order
---
# Channel order and codec slot conventions

## Scope

This reference records WAVE speaker order, WAV compatibility sets, the narrower FLAC format defaults, bounded ALAC channel declarations, and current Opus slot normalization.
[PCM source interpretation](../../system/playback/pcm-format.md) owns their playback applicability; slot conventions alone do not establish published source identity or native delivery.

## WAVE speaker positions

Identified application slots use ascending assigned WAVE speaker-bit order: the lowest set bit is the first interleaved channel.
[`SignalFormat.h`](../../../include/ao/audio/SignalFormat.h) defines the eighteen assigned position bits (`0x0003FFFF`), excluding reserved positions and `SPEAKER_ALL`.
Channel count, not mask population, owns frame stride; zero assigns no speaker positions.
See Microsoft's [WAVEFORMATEXTENSIBLE reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible) for raw ordering, excess-bit, and direct-out semantics.

## Canonical role sets

These sets support Aobus's [WAV compatibility policy](../../system/playback/pcm-format.md#wav-source-interpretation).
FLAC uses the same format-default identities only for counts 1, 2, 3, 4, 7, and 8; absent a mask declaration, five/six-channel FLAC remains unidentified as explained [below](#flac).
These sets are not defaults for unidentified PCM or native backend observations.

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

`FL`/`FR` are front left/right, `FC` is front center, and `LFE` is low-frequency effects.
`BL`/`BR` are back left/right, `BC` is back center, and `SL`/`SR` are side left/right.
`FLC`/`FRC` are front left/right of center.

## ALAC

The [ALAC source policy](../../system/playback/pcm-format.md#alac-source-interpretation) identifies only trustworthy mono/stereo absence defaults or complete supported explicit declarations.
The pinned [ALAC magic-cookie description](https://raw.githubusercontent.com/mikebrady/alac/5d8c5db0dfcadd5872f28e665cf4f4303447352a/ALACMagicCookieDescription.txt) explicitly leaves absent layouts above two channels unassigned.
The same revision's [ALACAudioTypes.h](https://raw.githubusercontent.com/mikebrady/alac/5d8c5db0dfcadd5872f28e665cf4f4303447352a/codec/ALACAudioTypes.h) supplies ALAC named tags.
Generic declarations are described by Apple's [CAF specification](https://developer.apple.com/library/archive/documentation/MusicAudio/Reference/CAFSpec/CAF_spec/CAF_spec.html) and [QuickTime audio channel layout atom](https://developer.apple.com/documentation/quicktime-file-format/audio_channel_layout_atom).
These are format/source references, not evidence of an executed reference encoder or physical speaker delivery.

### Bounded declarations and order

Aobus accepts a bounded union of full named tags, not a channel-count lookup.
Representative declarations below show raw provider-arrival roles and the zero-based source slot copied to each canonical destination.
They are explicit declarations, never defaults for untagged multichannel ALAC.

| Tag (identifier/count) | Raw roles | Effective mask | Destination-to-source slots |
|---|---|---:|---|
| Generic `114/3` (`0x00720003`) | FC FL FR | `0x007` | `[1,2,0]` |
| Shared `116/4` | FC FL FR BC | `0x107` | `[1,2,0,3]` |
| Shared `120/5` | FC FL FR BL BR | `0x037` | `[1,2,0,3,4]` |
| Shared `124/6` | FC FL FR BL BR LFE | `0x03F` | `[1,2,0,5,3,4]` |
| Shared `142/7` | FC FL FR BL BR BC LFE | `0x13F` | `[1,2,0,6,3,4,5]` |
| Shared `127/8` | FC FLC FRC FL FR BL BR LFE | `0x0FF` | `[3,4,0,7,5,6,1,2]` |

The four-channel ALAC reference layout is not quad; the seven-channel layout is not the WAV seven-channel compatibility set, and the eight-channel layout uses front-wide/back roles rather than WAV's side/back set.
Their masks are respectively `0x107`, `0x13F`, and `0xFF`, not count-derived WAV masks.
The exact accepted full-tag table lives in [`AlacDecoderSession.cpp`](../../../lib/audio/AlacDecoderSession.cpp), including mono/stereo and generic quad, MPEG 4.0 A, 5.0 A/B, 5.1 A/B/C, and 7.1 A alternatives.
This is not universal support for ALAC layouts or coding profiles.

Exact full tag `0x00710003` (`113/3`) means FC-FL-FR in ALAC but FL-FR-FC in generic Core Audio.
It is therefore `Unknown` everywhere, including a physical MP4 sibling `chan`: placement does not establish the namespace.
Channel count, the first packet's SCE/CPE shape, instance tags, producer guesses, and endian guesses never disambiguate it.

A generic sample-entry sibling `chan` can instead declare:

- Bitmap tag `0x00010000`: a nonzero complete supported WAVE set with one bit per configured channel, in ascending-bit order; no descriptions.
- Description tag zero: zero bitmap and exactly one complete 20-byte named description per configured channel, in array order.
  Only distinct labels 1–18 with zero flags are supported; labels 5/6 mean back left/right, and 10/11 mean side left/right.
  Labels 33/34 are unsupported, not back/side aliases.
  Unused label zero, coordinate-only label 100, discrete, flagged, duplicate, or unsupported labels do not identify a layout.
  Coordinate fields are ignored for supported named labels with zero flags, including nonzero coordinate values; they neither invalidate those labels nor supply role evidence.

Sibling parsing uses the actual compact or extended atom header and requires zero version/flags and exact declaration lengths.
Partial, reserved, wildcard, or zero bitmaps, count mismatches, unsupported tags/versions, and malformed lengths withhold identity.
Cookie-contained suffixes have a narrower grammar: only supported ALAC named tags in a compact 24-byte `chan`, with zero version/flags and two zero reserved words.
Those words are not a generic bitmap/description grammar, and generic-only `114/3` is not accepted there.
All these fields are big-endian.
The pinned reference `GetMagicCookie` writer's host-endian `memcpy` is not permission to accept an alternate byte-swapped grammar.

### Completeness and compatibility

All inspected cookie-contained and sibling declarations must agree on both order and set.
A single bad, conflicting, ambiguous, or uninspectable declaration makes identity terminally `Unknown`; a later valid declaration cannot repair it, even for mono/stereo.
Trustworthy absence requires a completely inspected associated entry and cookie suffix, not merely failure to find `chan`.
An unassociated entry, unsupported sample-entry version, duplicate cookie, or uninspectable optional tail withholds identity without introducing a new file gate.
Nested size-zero atoms remain invalid under the existing global parser: invalid required prefixes still fail, while previously unvisited optional tails only withhold identity.

For identified layouts, whole native sample bytes are permuted before precision-preserving output adaptation.
For `Unknown`, no role permutation occurs and legacy ordinal PCM remains unchanged.
These guarantees cover the bounded metadata and slot policy, not every ALAC coding profile, hardware route, or a numerical decoder replacement.
The [decoder lifecycle](../../system/playback/decoder-session.md#alac-metadata-and-pcm) owns the borrowed-entry lifetime and required-error precedence.

## FLAC

When metadata inspection is complete and trustworthy and no exact standardized channel-mask key is present, FLAC uses the sets above with `Identified` source interpretation for counts 1, 2, 3, 4, 7, and 8.
Five/six-channel FLAC instead retains zero effective masks and `Unknown` origin: [RFC 9639 §9.1.3](https://www.rfc-editor.org/rfc/rfc9639.html#section-9.1.3) does not resolve its back/surround pair into WAVE BL/BR versus SL/SR.
This is not an `Inferred` fallback and does not change WAV's five/six-channel compatibility sets.
Untrustworthy or incomplete optional identity evidence, including malformed, conflicting, zero, partial, excess, reserved-bit, or wildcard declarations, likewise leaves both effective masks zero and origin `Unknown`; later valid declarations cannot repair it.
The format-default masks, including FC mono, are Aobus's canonical representation, not verbatim RFC WAVE masks or physical-geometry guarantees.
Complete supported agreeing `WAVEFORMATEXTENSIBLE_CHANNEL_MASK` comments identify their declared roles in ascending assigned-bit order, not arbitrary slot swaps.
Explicit five/six-channel back masks `0x37`/`0x3F` and side masks `0x607`/`0x60F` remain distinct identified sets.
libFLAC already returns logical decoded slots; the session adds no PCM reorder.
The [owning PCM policy](../../system/playback/pcm-format.md#flac-source-interpretation) defines comment parsing, duplicate resolution, best-effort admission, and the distinction from FLAC validity and streamable-subset membership.
Unknown playback does not certify intended speaker roles or native routing; numerical bit-perfectness is independent of channel confidence.
None of these source identities proves native delivery or physical wiring.

## Opus

[Ogg Opus](https://www.rfc-editor.org/rfc/rfc7845.txt) defines channel mapping families with different slot orders:

- Family 0 is already compatible with mono/stereo WAVE order.
- Family 1 orders channels the way Vorbis does, so the session permutes the mapping it hands libopus into WAVE speaker order.
  The canonical destination-to-decoder permutations are `3:[0,2,1]`, `5:[0,2,1,3,4]`, `6:[0,2,1,5,3,4]`, `7:[0,2,1,6,5,3,4]`, and `8:[0,2,1,7,5,6,3,4]`; counts 1, 2, and 4 are identity.
- Family 255 defines no speaker positions and may expose up to 255 unassigned channels in header-defined output-index order.

These byte-order mechanisms do not publish source identity: the current Opus session still reports a zero speaker mask and `Unknown` layout origin.
[`OpusDecoderSession.cpp`](../../../lib/audio/OpusDecoderSession.cpp) owns the mapping, covered by the family-1 speaker-order case in [`OpusDecoderSessionTest.cpp`](../../../test/unit/audio/OpusDecoderSessionTest.cpp).

## Related documents

- [PCM representation and lossless output](../../system/playback/pcm-format.md)
- [Decoder session](../../system/playback/decoder-session.md)
