# AAC channel-identity fixtures

These short AAC-LC MP4 files exercise native output identity, not gapless playback or hardware routing.
They were generated offline with pinned-Nix FFmpeg 9.0.1 (GCC 16.2.0); no encoder runs in the tests.
Each input is 12,288 frames of interleaved signed-16 PCM at 48,000 Hz.
For source slot `c` and frame `n`, the sample is Python's `round(8192 * sin(2 * pi * (300 + 200*c) * n / 48000))`, packed little-endian.
The explicit input layout below assigns those slots their authored roles.

The encoder command, with `channels * 64000` as the bitrate, was:

```sh
ffmpeg -hide_banner -nostdin -loglevel error -fflags +bitexact \
  -channel_layout "$layout" -i "$input_wav" -map_metadata -1 \
  -c:a aac -aac_pce 1 -b:a "$bitrate" -flags:a +bitexact "$output_m4a"
```

| File | Authored layout | Native FDK output groups | Effective identity |
|---|---|---|---|
| `mono.m4a` | mono | FRONT1 | FC `0x4` |
| `stereo.m4a` | stereo | FRONT2 | FL/FR `0x3` |
| `front3.m4a` | 3.0 | FRONT3 | FL/FR/FC `0x7` |
| `back3.m4a` | 3.0(back) | FRONT2 + BACK1 | FL/FR/BC `0x103` |
| `quad.m4a` | quad | FRONT2 + BACK2 | FL/FR/BL/BR `0x33` |
| `back51.m4a` | 5.1 | FRONT3 + LFE1 + BACK2 | FL/FR/FC/LFE/BL/BR `0x3F` |
| `surround71.m4a` | 7.1 | FRONT3 + LFE1 + BACK4 | Zero / Unknown |
| `wide71.m4a` | 7.1(wide) | FRONT5 + LFE1 + BACK2 | Zero / Unknown |

The two eight-channel cases intentionally do not infer WAVE roles from count or the input filename.
They retain all eight native channels and ordinal PCM because the bounded resolver does not assign FRONT5 or BACK4 groups.
A separately generated `5.1(side)` input produced the same encoded file and FDK BACK2 reporting as `5.1`, so it was not retained as independent evidence for SIDE2.

A standalone upstream x86-64 FDK-AAC 2.0.3 decoder, without Aobus code, supplied the literal PCM windows and nonzero counts in `AacDecoderChannelLayoutTest.cpp`.
It used raw MP4 packet payloads, `TT_MP4_RAW`, WAV mapping 1, disabled channel downmix/extension, and the otherwise-default decoder parameters.
All files contain 13 packets yielding 1,024 PCM frames each: 13,312 returned frames, including the decoder's initial delay.
The retained MP4 duration/edit metadata is not a newly implemented AAC delay/padding trim contract.

Native defaults are not identical merely because both packages are named FDK-AAC 2.0.3.
The measured upstream Linux x86-64 build reports 1,744 added-delay frames for these fixtures; observed Intel and ARM64 macOS vcpkg `2.0.3#2` LC-only builds report 720.
The measured Intel fork lacks the upstream interpolating-concealment frame of delay, even when setting concealment method 2 returns success.
For the compared Linux and Intel macOS builds, identical packet/configuration bytes yield twelve exactly matching overlapping PCM packets after that one-packet shift; explicitly selecting method 1 on upstream matches all thirteen Intel fork packets.
That measured exactness is not a cross-architecture guarantee.
This explains why the upstream's first returned packet is entirely silent while the fork's first packet already contains some nonzero PCM.

The literal windows and nonzero counts are unchanged: their fixture phase is 304 frames after the reported native added delay, at packet 2 for 1,744 or packet 1 for 720.
Tests require one of those measured delays and the exact packet boundary, with no sample search.
Only the two 32-sample cross-implementation reference-window comparisons allow an absolute difference of one native signed-16 LSB; all literal values and exact nonzero counts are retained.
This bound follows an observed ARM64 `front3.m4a` packet-1 sample at zero-based index 21: native and Aobus both return 21, while the x86-64 reference is 22.
The precise architecture/compiler arithmetic cause has not been established; this is not permission to alter production PCM or decoder parameters.
A separate file-local raw FDK decode supplies complete packet PCM for first-block preservation, contiguous output, flush and pristine initial-seek checks, all compared exactly on the executing host.
Every integer and floating-point output-adaptation comparison also remains exact.
It does not use Aobus channel planning or output adaptation; identified role expectations remain literal.
This test-only phase coordinate neither changes production decoder parameters nor trims delay/padding, and does not enable vcpkg's optional HE-AAC feature.
These fixed-point expectations still require validation on every claimed native implementation.

| File | SHA-256 |
|---|---|
| `mono.m4a` | `2447956523e45ada84d65f6580d49dea44908d784877fe80e4143369abc38d54` |
| `stereo.m4a` | `bbc669f5fa4ddc02a11eb4739a4bc44cab0150899f73159be42bec278a3e8809` |
| `front3.m4a` | `85034bd6ea43f84c4d06e08264b97f6e872b8e5256a8eb3532e4f100302f4ea1` |
| `back3.m4a` | `558219645079687057f6d33b42e2d6f93daf1bf8e7a0a7f967a3dfbd55824361` |
| `quad.m4a` | `8b908b0a57ec8efd4ffe6921de567708a43a20b7a6544ef8e42d4390238ef65b` |
| `back51.m4a` | `fc103eb020c7aa132e663cb8c8db7880dad8dea7731036e9ce0db35779e88999` |
| `surround71.m4a` | `ee6b49e739272a195d09fac196900336d359d6d210d5a970cea98ebc902e5452` |
| `wide71.m4a` | `19893c6dd10c72cd98861e077dac8b0806b424ab040d867d1d08944c22cb2a93` |
