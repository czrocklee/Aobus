---
id: media.channel-order
---
# Channel order and codec slot conventions

## Scope

This reference records slot conventions and destination-to-source permutations.
[PCM source interpretation](../../system/playback/pcm-format.md) owns their playback applicability.

## WAVE speaker positions

Application slots use ascending assigned WAVE speaker-bit order: the lowest set bit is the first interleaved channel.
[`SignalFormat.h`](../../../include/ao/audio/SignalFormat.h) defines the eighteen assigned position bits, excluding reserved positions and `SPEAKER_ALL`.
See Microsoft's [WAVEFORMATEXTENSIBLE reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible) for raw ordering, excess-bit, and direct-out semantics.

## Canonical role sets

These sets support Aobus's [WAV compatibility policy](../../system/playback/pcm-format.md#wav-source-interpretation).
[RFC 9639](https://www.rfc-editor.org/rfc/rfc9639.html#section-9.1.3) establishes the exact FLAC sets for counts 1, 2, 3, 4, 7 and 8; its generic five/six-channel surround pair does not select the exact BACK sets below.

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

## Opus

[Ogg Opus](https://www.rfc-editor.org/rfc/rfc7845.txt) defines channel mapping families with different slot orders:

- Family 0 is already compatible with the canonical role sets.
- Family 1 orders channels the way Vorbis does, so the session permutes the mapping it hands libopus into the WAVE speaker order every PCM surface expects.
  The canonical destination-to-decoder permutations are `3:[0,2,1]`, `5:[0,2,1,3,4]`, `6:[0,2,1,5,3,4]`, `7:[0,2,1,6,5,3,4]`, and `8:[0,2,1,7,5,6,3,4]`; counts 1, 2, and 4 are identity.
- Family 255 defines no speaker positions and may expose up to 255 unassigned channels in header-defined output-index order.

## ALAC

The [ALAC reference convention](https://github.com/mikebrady/alac/blob/5d8c5db0dfcadd5872f28e665cf4f4303447352a/codec/ALACAudioTypes.h) differs from canonical WAVE slot order:

| Count | Reference order | Canonical destination-to-source indices | Set |
|---:|---|---|---:|
| 3 | C L R | `1,2,0` | `0x007` |
| 4 | C L R Cs | `1,2,0,3` | `0x107` |
| 5 | C L R Ls Rs | `1,2,0,3,4` | `0x037` |
| 6 | C L R Ls Rs LFE | `1,2,0,5,3,4` | `0x03F` |
| 7 | C L R Ls Rs Cs LFE | `1,2,0,6,3,4,5` | `0x13F` |
| 8 | C Lc Rc L R Ls Rs LFE | `3,4,0,7,5,6,1,2` | `0x0FF` |

These multichannel permutations are not implemented by the current ALAC session.
ALAC and generic Core Audio/CAF tag namespaces differ; matching counts, masks or numeric tags alone do not establish the same slot order.
In particular, reference ALAC four-channel is not quad, seven-channel is not WAV seven-channel, and eight-channel includes wide/back roles.

## Related documents

- [PCM representation and lossless output](../../system/playback/pcm-format.md)
- [Decoder session](../../system/playback/decoder-session.md)
