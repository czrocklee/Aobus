---
id: media.channel-order
---
# Channel order and codec slot conventions

## Scope

This reference records WAVE speaker order, WAV compatibility role sets, and current Opus slot normalization.
[PCM source interpretation](../../system/playback/pcm-format.md) owns their playback applicability; slot conventions alone do not establish published source identity or native delivery.

## WAVE speaker positions

Identified application slots use ascending assigned WAVE speaker-bit order: the lowest set bit is the first interleaved channel.
[`SignalFormat.h`](../../../include/ao/audio/SignalFormat.h) defines the eighteen assigned position bits (`0x0003FFFF`), excluding reserved positions and `SPEAKER_ALL`.
Channel count, not mask population, owns frame stride; zero assigns no speaker positions.
See Microsoft's [WAVEFORMATEXTENSIBLE reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible) for raw ordering, excess-bit, and direct-out semantics.

## Canonical role sets

These sets support Aobus's [WAV compatibility policy](../../system/playback/pcm-format.md#wav-source-interpretation).
They are not defaults for unidentified PCM or native backend observations.

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
