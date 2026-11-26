# AAC seek fixtures

Both fixtures use original synthetic signals generated for Aobus; neither contains a third-party recording.

## Single-rate HE-AAC

`he-single-rate.m4a` distinguishes HE-AAC history from AAC-LC transform overlap. Upstream FDK decodes these access units with a 32 kHz core and output rate, 1024 frames per packet, and an SBR extension. Rate and frame length alone therefore do not justify a short AAC-LC restart. The Linux seek test compares PCM at 641 ms (32 frames into packet 20) to an uninterrupted decode by the same provider. It does not require HE-AAC support from the LC-only vcpkg build.

The fixture was generated offline with FDK-AAC 2.0.3 and libavformat from FFmpeg 9.0.2. The encoder input is mono, signed 16-bit PCM at 32 kHz. For frame `n` starting at zero, set `w = 2*pi*n/32000` and truncate `14000*(sin(1300*w) + 0.45*sin(9100*w) + 0.6*sin(11700*w))` toward zero. Feed 1024 samples per call with these FDK settings; leave other settings at their defaults:

| Parameter               | Value        |
| ----------------------- | ------------ |
| `AACENC_AOT`            | `5`          |
| `AACENC_SBR_RATIO`      | `1`          |
| `AACENC_SAMPLERATE`     | `32000`      |
| `AACENC_CHANNELMODE`    | `MODE_1`     |
| `AACENC_BITRATE`        | `40000`      |
| `AACENC_TRANSMUX`       | `TT_MP4_RAW` |
| `AACENC_SIGNALING_MODE` | `2`          |

Mux the first 80 emitted access units without a flush using the encoder's AudioSpecificConfig. The `ipod` muxer uses bitexact mode and a `1/32000` audio time base; each packet has duration 1024 and DTS = PTS starting at zero. No encoder or muxer runs in the test.

## AAC-LC with a mid-stream DRC declaration

`lc-drc.m4a` contains 15 plain AAC-LC access units followed by 15 with MPEG dynamic-range metadata. Both runs use fresh FDK-AAC 2.0.3 encoders, raw transport, 48 kHz stereo, 128 kbit/s, and the same AudioSpecificConfig (`11 90`). Both channels carry the same 1 kHz tone: truncate `12000*sin(2*pi*1000*n/48000)` toward zero for each signed-16 input sample, starting at `n = 0` for each encoder. Keep the first 15 emitted packets per run, without flushing.

The second encoder sets `AACENC_METADATA_MODE=1` and supplies `AACENC_MetaData` with `drc_profile=3` (Music Standard), `comp_profile=1`, both target reference levels `-24*65536`, `prog_ref_level_present=1`, and `prog_ref_level=-6*65536`; other metadata fields are zero. The `ipod` muxer concatenates the raw packets on a `1/48000` time base with 1024 ticks per packet. Packet 15 changes the DRC flags, not the output rate, channel roles, or frame length.

## Checksums

| File                 | SHA-256                                                            |
| -------------------- | ------------------------------------------------------------------ |
| `he-single-rate.m4a` | `041dc5989081187a0157c4542423dd809a6a9e55a8c5993c3d1c7feffdcd90df` |
| `lc-drc.m4a`         | `6926397eb2ccd7363283467cfed116ae63687303e91868d116ed3c8c695f0cd6` |
