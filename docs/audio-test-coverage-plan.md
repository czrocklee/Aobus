# Audio Test Coverage Plan

Date: 2026-05-04

## Goal

Maximize meaningful automated coverage for `lib/audio` with this order of priority:

- deterministic pure/unit tests first
- state-machine tests with fakes second
- codec/file integration tests third
- real PipeWire/ALSA smoke tests last and opt-in where needed

The end state should make every important user-visible behavior in decoding, PCM buffering, engine/player orchestration, format negotiation, and backend/provider graph handling either:

- covered by hermetic automation, or
- explicitly marked as manual/environment-dependent.

## Scope

Primary implementation files:

- `lib/audio/FormatNegotiator.cpp`
- `lib/audio/DecoderFactory.cpp`
- `lib/audio/MemorySource.cpp`
- `lib/audio/PcmRingBuffer.cpp`
- `lib/audio/StreamingSource.cpp`
- `lib/audio/FlacDecoderSession.cpp`
- `lib/audio/AlacDecoderSession.cpp`
- `lib/audio/Engine.cpp`
- `lib/audio/Player.cpp`
- `lib/audio/backend/AlsaProvider.cpp`
- `lib/audio/backend/AlsaExclusiveBackend.cpp`
- `lib/audio/backend/PipeWireProvider.cpp`
- `lib/audio/backend/PipeWireBackend.cpp`
- `lib/audio/backend/PipeWireMonitor.cpp`
- `lib/audio/backend/detail/PipeWireShared.cpp`
- `include/ao/audio/PcmConverter.h`

Current audio-related test files:

- `test/unit/audio/FormatNegotiatorTest.cpp`
- `test/unit/audio/PcmConverterTest.cpp`
- `test/unit/audio/PlaybackEngineTest.cpp`
- `test/unit/audio/PlayerTest.cpp`
- `test/integration/audio/DecoderIntegrationTest.cpp`
- `test/integration/audio/GraphIntegrityTest.cpp`
- `test/integration/audio/GraphAnalysisVerification.cpp`
- `test/integration/audio/backend/PipeWireProviderTest.cpp`

Recommended new test files:

- `test/unit/audio/DecoderFactoryTest.cpp`
- `test/unit/audio/PcmRingBufferTest.cpp`
- `test/unit/audio/MemorySourceTest.cpp`
- `test/unit/audio/StreamingSourceTest.cpp`
- `test/unit/audio/backend/PipeWireSharedTest.cpp`
- `test/unit/audio/backend/PipeWireMonitorTest.cpp`
- `test/unit/audio/backend/PipeWireProviderTest.cpp`
- `test/unit/audio/backend/PipeWireBackendTest.cpp`
- `test/unit/audio/backend/AlsaProviderTest.cpp`
- `test/unit/audio/backend/AlsaExclusiveBackendTest.cpp`

## Current Validation Baseline

The existing debug test binary already available at `/tmp/build/debug/test/ao_test` was used as the baseline for this audit.

Current results:

- `/tmp/build/debug/test/ao_test '[playback],[format_negotiator],[pipewire]'`
  - 15 test cases
  - 8398 assertions
  - all passing
- `/tmp/build/debug/test/ao_test '[audio]'`
  - 4 test cases
  - 13 assertions
  - all passing

That mismatch is important:

- audio-related tests are currently tagged inconsistently
- filtering by `[audio]` misses most `Engine`, `Player`, decoder, and `FormatNegotiator` coverage
- the suite is broader than it first appears, but still far from sufficient for `lib/audio`

## Coverage Heatmap

Source line counts below come from the current tree.

| Module | LOC | Current automated coverage | Gap level |
| --- | ---: | --- | --- |
| `FormatNegotiator.cpp` | 245 | one unit suite with good happy-path matrix | medium |
| `PcmConverter.h` | header-only | basic happy paths only | medium |
| `DecoderFactory.cpp` | 30 | none | high |
| `MemorySource.cpp` | 142 | none | high |
| `PcmRingBuffer.cpp` | 48 | none | high |
| `StreamingSource.cpp` | 305 | none | critical |
| `FlacDecoderSession.cpp` | 421 | light positive integration only | high |
| `AlacDecoderSession.cpp` | 262 | light positive integration only | critical |
| `Engine.cpp` | 865 | partial unit coverage plus light integration | critical |
| `Player.cpp` | 691 | partial unit coverage | critical |
| `AlsaProvider.cpp` | 329 | none | high |
| `AlsaExclusiveBackend.cpp` | 507 | none | critical |
| `PipeWireProvider.cpp` | 87 | one real-daemon smoke test only | medium |
| `PipeWireBackend.cpp` | 452 | none | critical |
| `PipeWireMonitor.cpp` | 1305 | only indirect real-daemon smoke | critical |
| `PipeWireShared.cpp` | 109 | none | high |

Total `lib/audio` implementation under test pressure is roughly 5864 LOC, with the largest gaps concentrated in:

- source buffering and threading
- backend callback state machines
- PipeWire graph/capability parsing
- ALSA device handling and recovery

## Existing Fixture Inventory

The current repository already has reusable audio fixtures under `test/integration/tag/test_data`.

| Fixture | Verified codec/format | Recommended use |
| --- | --- | --- |
| `basic_metadata.flac` | FLAC / 44100 Hz / 2 ch / 16-bit | basic positive FLAC open, metadata, engine graph |
| `hires.flac` | FLAC / 96000 Hz / 2 ch / 24-bit | hi-res format negotiation and decoder reopen cases |
| `hires.m4a` | ALAC / 96000 Hz / 2 ch / 24-bit | positive ALAC open, bit-perfect ALAC conversion |
| `basic_metadata.m4a` | AAC / 44100 Hz / 2 ch | negative fixture for `AlacDecoderSession` and `Engine` decoder-open failure |
| `with_cover.m4a` | AAC / 44100 Hz / 2 ch | secondary negative AAC-with-cover fixture |
| `empty.flac` | FLAC / 44100 Hz / 2 ch / 16-bit | minimal positive FLAC edge cases |
| `basic_metadata.mp3` | MP3 / unsupported by `DecoderFactory` | negative fixture for unsupported-extension engine paths |

Notes:

- `basic_metadata.m4a` is valuable because it proves `.m4a` is not equivalent to ALAC in this repository.
- Current audio tests reuse `TAG_TEST_DATA_DIR`, which is workable but mixes tag and audio concerns.

## New Fixtures To Add

Keep file fixtures small. Prefer fake/scripted decoders for large-data or failure-path coverage.

| Fixture | How to create | Purpose |
| --- | --- | --- |
| `truncated_basic.flac` | copy first 256-512 bytes of `basic_metadata.flac` | malformed FLAC open/decode failure |
| `truncated_hires.m4a` | copy first 256-512 bytes of `hires.m4a` | malformed ALAC/MP4 parse failure |
| `mono_48k.flac` | generate with `ffmpeg` mono 48 kHz FLAC | mono channel/path edge cases |
| `surround_5_1.flac` | generate with `ffmpeg` 5.1 FLAC | channel-remap rejection and graph expectations |

Avoid committing a very large PCM fixture just to force `StreamingSource` selection. That branch is better covered with a scripted decoder and a large `durationMs` in fake `DecodedStreamInfo`.

## Reusable Test Harness

### Immediate Dispatcher

Use a tiny `IMainThreadDispatcher` fake that executes callbacks synchronously. Current `MockDispatcher` and `ImmediateDispatcher` patterns are already good seeds.

### ScriptedDecoderSession

Add a reusable fake `IDecoderSession` with configurable:

- `open()` result
- `seek()` result
- `streamInfo()` value
- `readNextBlock()` script
- call counters for `open`, `close`, `seek`, `flush`, `readNextBlock`

This unlocks most missing coverage in:

- `MemorySource`
- `StreamingSource`
- `Engine`

### CapturingBackend

Add a reusable fake `IBackend` that:

- records `open/start/pause/resume/flush/drain/stop/reset/close`
- stores the last `RenderCallbacks`
- lets tests manually fire `onRouteReady`, `onFormatChanged`, `onBackendError`, `onDrainComplete`

### PipeWire Pod Builder Helper

Add a small helper for constructing `spa_pod` format and property payloads in tests. That gives cheap coverage for:

- `parseRawStreamFormat()`
- `parseEnumFormat()`
- sink property parsing

### Optional Thin C-API Seams

If backend callback logic proves too hard to cover directly, add tiny internal shims rather than full wrappers:

- an `AlsaApi` table for `snd_pcm_*` calls used by `AlsaExclusiveBackend`
- a `PipeWireStreamApi` or test seam for the `pw_stream_*` calls used by `PipeWireBackend`

## Behavior Decisions To Lock Before Generating Tests

### Decision 1

All audio-related tests should carry a consistent superset of tags:

- `[audio]`
- layer tag such as `[unit]` or `[integration]`
- feature tags such as `[engine]`, `[player]`, `[pipewire]`, `[alsa]`, `[codec]`

Reason: today `[audio]` only selects a small fraction of relevant coverage.

### Decision 2

`GraphAnalysisVerification.cpp` should become an opt-in manual/debug test, not a default automation participant.

Reason: it uses a hardcoded absolute path under `/home/rocklee/Music/song_1.flac` and is currently only "passing" because it skips itself.

### Decision 3

`AlacDecoderSession::seek(positionMs)` should honor the requested position.

Recommended behavior:

- seeking to 0 ms returns first-frame data
- seeking to a later position changes the next decoded block accordingly

Reason: the current implementation resets `_currentSampleIndex` to 0 regardless of `positionMs`.

### Decision 4

`parseUintProperty()` should reject partially numeric strings such as `"12abc"`.

Recommended behavior:

- accept only full decimal strings
- reject null, empty, non-numeric, and partially numeric text

Reason: partial parse success makes PipeWire property bugs harder to detect.

### Decision 5

The `Player` should resubscribe to the graph source if the route anchor changes within the same playback generation.

Reason: current logic only subscribes when `_impl->graphSubscription` is empty.

## Recommended Test Structure

| File | Action | Reason |
| --- | --- | --- |
| `test/unit/audio/FormatNegotiatorTest.cpp` | expand | finish branch coverage of negotiation fallbacks |
| `test/unit/audio/PcmConverterTest.cpp` | expand | cover guard/truncation/sign-extension edges |
| `test/unit/audio/PlaybackEngineTest.cpp` | expand heavily | most user-visible control-plane risk lives here |
| `test/unit/audio/PlayerTest.cpp` | expand heavily | quality analysis and graph subscription logic are central |
| `test/integration/audio/DecoderIntegrationTest.cpp` | expand | keep real codec confidence for FLAC + ALAC |
| `test/integration/audio/backend/PipeWireProviderTest.cpp` | keep as smoke and expand carefully | still useful for daemon integration confidence |
| `test/unit/audio/DecoderFactoryTest.cpp` | add | trivial but missing contract |
| `test/unit/audio/PcmRingBufferTest.cpp` | add | concurrency primitive currently untested |
| `test/unit/audio/MemorySourceTest.cpp` | add | pure, high-value buffer logic |
| `test/unit/audio/StreamingSourceTest.cpp` | add | biggest missing threading/state risk |
| `test/unit/audio/backend/PipeWireSharedTest.cpp` | add | cheap high-value parsing coverage |
| `test/unit/audio/backend/PipeWireMonitorTest.cpp` | add after extracting helper seams | covers most of the 1305 LOC monitor logic without daemon dependency |
| `test/unit/audio/backend/PipeWireProviderTest.cpp` | add | provider behavior should not rely only on real daemon smoke |
| `test/unit/audio/backend/PipeWireBackendTest.cpp` | add with tiny seam | callback state machine is too important to leave untested |
| `test/unit/audio/backend/AlsaProviderTest.cpp` | add with enumeration seam | provider contract is otherwise uncovered |
| `test/unit/audio/backend/AlsaExclusiveBackendTest.cpp` | add with ALSA shim | high-risk recovery and format-fallback logic |

## Detailed Test Matrix

### Format Negotiation

Target file: `test/unit/audio/FormatNegotiatorTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Keep | `FormatNegotiator - Build Plan` | Preserve the current matrix for passthrough, resample requirement, bit-depth conversion, channel remap, 24-bit negotiation, and 32-bit negotiation | current `sourceFormat` and capability tables already in the file | current assertions remain the baseline contract |
| Add | `[P0] FormatNegotiator - 16-bit source pads to 32/16 when only 32-bit container exists` | cover legacy 16-bit fallback path not currently asserted | `source={44100,2,16,16,false,true}`, `caps.bitDepths={32}`, `caps.sampleFormats={}` or a non-matching set, `caps.channelCounts={2}`, `caps.sampleRates={44100}` | `decoderOutputFormat=32/16`, `deviceFormat=32/16`, bit-depth conversion required |
| Add | `[P1] FormatNegotiator - Reason string concatenates multiple required conversions in stable order` | prove multi-flag reason assembly | `source={44100,2,16,16,false,true}`, `caps.sampleRates={48000}`, `caps.bitDepths={32}`, `caps.channelCounts={6}` | reason contains `sample rate resampling required; bit depth conversion required; channel remapping required` |
| Add | `[P1] FormatNegotiator - Empty sampleFormats still chooses a safe 24-bit fallback` | cover legacy branch when `sampleFormats` is empty | `source={96000,2,24,24,false,true}`, `caps.sampleFormats={}`, `caps.bitDepths={32}`, `caps.sampleRates={96000}`, `caps.channelCounts={2}` | `decoderOutputFormat=32/24`, `deviceFormat=32/24`, no crash |

### PCM Utilities

Target file: `test/unit/audio/PcmConverterTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Keep | `PcmConverter: pad (Linear)` | preserve current 16-to-32 and 24-to-32 padding checks | current integer arrays | current shifted sample values stay correct |
| Keep | `PcmConverter: interleaveAndPad` | preserve current stereo interleaving check | current `left/right` spans | interleaved destination remains `L0,R0,L1,R1` |
| Keep | `PcmConverter: unpackS24` | preserve current 24-bit unpack + shift check | current packed byte array | values remain correctly sign-extended and shifted |
| Add | `[P1] PcmConverter - pad copies only the common sample count` | cover `std::min(source.size(), destination.size())` branch | source has 3 samples, destination has 2 slots | only first 2 outputs are written; no overflow |
| Add | `[P1] PcmConverter - interleaveAndPad returns immediately for empty channel list` | cover guard branch | empty `channels`, prefilled destination sentinel values | destination stays unchanged |
| Add | `[P1] PcmConverter - interleaveAndPad truncates to destination frame capacity` | cover short destination handling | 2-channel input with 3 frames each, destination capacity for 2 frames only | only 4 destination samples are written |
| Add | `[P1] PcmConverter - unpackS24 sign-extends negative full-scale values without extra shift` | strengthen 24-bit sign-extension coverage | packed bytes for `0x800000`, `0xFFFFFF`, `0x7FFFFF` with `shift=0` | outputs become `0xFF800000`, `-1`, `0x007FFFFF` |

### Decoder Factory

Target file: `test/unit/audio/DecoderFactoryTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Add | `[P0] DecoderFactory - Creates FLAC session for .flac` | lock supported extension contract | `song.flac`, any output format | returns non-null `FlacDecoderSession` |
| Add | `[P0] DecoderFactory - Creates ALAC session for .m4a and .mp4` | lock supported extension contract | `song.m4a`, `song.mp4`, any output format | returns non-null `AlacDecoderSession` |
| Add | `[P0] DecoderFactory - Returns null for unsupported extensions` | cover negative branch | `song.mp3`, `song.wav`, `song.ogg` | returns null |
| Add | `[P1] DecoderFactory - Case-sensitive extension behavior is explicit` | force a product decision instead of silent ambiguity | `song.FLAC` | either returns null and the test documents current behavior, or code is changed first and the test is updated to the desired case-insensitive contract |

### Ring Buffer

Target file: `test/unit/audio/PcmRingBufferTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Add | `[P0] PcmRingBuffer - Empty read and write are no-ops` | cover trivial guards | empty input span and empty output span | returned byte count is 0 in both cases |
| Add | `[P0] PcmRingBuffer - Preserves FIFO order across multiple writes and reads` | core contract | write byte ranges `A`, `B`, read in smaller chunks | output bytes match `A+B` in order |
| Add | `[P1] PcmRingBuffer - Partial read leaves remaining bytes available` | prove `size()` bookkeeping | write 10 bytes, read 4, then read 6 | first read returns 4, second read returns the remaining 6 |
| Add | `[P1] PcmRingBuffer - Capacity limit causes short write instead of overflow` | cover saturation | write `kRingBufferCapacity + 64` bytes | `write()` returns a value `< input.size()`, stored data remains valid |
| Add | `[P1] PcmRingBuffer - Clear resets size and supports reuse` | cover `clear()` behavior | write bytes, call `clear()`, then write/read a new pattern | size becomes 0 after clear and the second pattern round-trips correctly |
| Add | `[P2] PcmRingBuffer - Single producer and single consumer smoke test` | concurrency sanity check | one producer thread writes numbered chunks, one consumer thread reads until all bytes received | total byte count and byte order remain intact |

### Memory Source

Target file: `test/unit/audio/MemorySourceTest.cpp`

Recommended fake data catalog for this file:

- `Mono16_1k`: output format `1000 Hz / 1 ch / 16-bit`, so 2 bytes == 1 ms
- `Stereo24_1k`: output format `1000 Hz / 2 ch / 24-bit`, so 6 bytes == 1 ms

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Add | `[P0] MemorySource - Initialize buffers all blocks until EOS and closes decoder` | positive initialization path | `ScriptedDecoderSession` returns two PCM blocks then EOS, `streamInfo=Mono16_1k` with `durationMs=10` | `initialize()` succeeds, decoder `close()` called once, total buffered bytes equal concatenated blocks |
| Add | `[P0] MemorySource - Initialize converts decoder failure to DecodeFailed` | error propagation path | first `readNextBlock()` returns `Error{DecodeFailed,"decoder read failed"}` | `initialize()` returns `DecodeFailed` with propagated message |
| Add | `[P0] MemorySource - Read returns bytes sequentially until drained` | core read semantics | preloaded 3 short blocks under `Mono16_1k` | repeated `read()` returns bytes in order and `isDrained()` flips true at the end |
| Add | `[P1] MemorySource - BufferedMs tracks remaining PCM bytes` | duration math branch | 8 bytes loaded under `Mono16_1k`; read 4 bytes | `bufferedMs()` goes from 4 ms to 2 ms |
| Add | `[P1] MemorySource - Seek aligns 24-bit stereo offsets to frame size and clamps at end` | cover `positionToByteOffset()` branch | `Stereo24_1k`, 24 bytes total, seek to 1 ms then beyond file end | first seek lands on byte offset 6, oversized seek clamps to full buffer size |
| Add | `[P1] MemorySource - Zeroed output format yields zero buffered duration and zero seek offset` | guard branch | `streamInfo.outputFormat={0,0,0,0,false,true}` | `bufferedMs()==0`, `seek()` succeeds without moving out of bounds |

### Streaming Source

Target file: `test/unit/audio/StreamingSourceTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Add | `[P0] StreamingSource - Initialize matrix` | cover preroll success, EOF-during-preroll success, and preroll decode failure | use `ScriptedDecoderSession` variants under `Mono16_1k`; one returns enough data, one returns EOS immediately, one returns `DecodeFailed` during preroll | success starts a thread only when not EOF, EOF path succeeds without thread, failure path returns error and fires `onError` once |
| Add | `[P0] StreamingSource - Seek matrix` | cover stop/restart behavior and seek failure | start with a decoder that can preroll, then seek to 5 ms; separate variant returns `SeekFailed` | successful seek clears ring buffer, calls decoder `seek(5)`, re-prerolls data; failed seek returns error and reports exactly once |
| Add | `[P1] StreamingSource - Background decode failure after initialization marks source failed` | async error path | first preroll block succeeds, later `readNextBlock()` returns `DecodeFailed` on background thread | `onError` is called once, future `initialize/seek` observations see failed state |
| Add | `[P1] StreamingSource - Large block write loops until all bytes are queued or stop is requested` | cover partial ring writes | fake decoder emits a block larger than ring capacity | no data corruption, write loop exits cleanly when remaining bytes reach 0 or stop token is set |
| Add | `[P1] StreamingSource - BufferedMs and isDrained track EOF plus ring exhaustion` | core source semantics | decoder emits a small preroll block, then EOS; test consumes buffered bytes via `read()` | `isDrained()` becomes true only after EOF is known and the ring buffer is empty |

### FLAC Decoder

Target files: `test/integration/audio/DecoderIntegrationTest.cpp`, optionally `test/unit/audio/FlacDecoderSessionTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Keep | `Decoder Bit-Perfect Conversions` | preserve current FLAC 16-to-32 bit-perfect padding check | `basic_metadata.flac` opened once as 16-bit output and once as 32-bit output | 32-bit samples equal 16-bit samples shifted left by 16 |
| Keep | `FLAC Decoder Integrity` | preserve current metadata and seek consistency checks | `basic_metadata.flac` | metadata stays `44100/2/16`, seek returns a non-empty block |
| Add | `[P0] FlacDecoderSession - Open and seek error matrix` | cover missing file and pre-open seek failure | missing file path; separate `seek(100)` before `open()` | `open()` returns error, pre-open seek returns `SeekFailed` with `Sample rate is 0` |
| Add | `[P1] FlacDecoderSession - EOF and unsupported output matrix` | cover repeated EOF and unsupported requested bit depth | read until EOS on `basic_metadata.flac`; separate open with `requestedOutput.bitDepth=8` | repeated reads keep returning `endOfStream=true`; unsupported bit depth fails decoding instead of producing garbage |
| Add | `[P1] FlacDecoderSession - Non-FLAC fixture is rejected` | malformed/wrong-container path | `basic_metadata.mp3` or `truncated_basic.flac` opened through `FlacDecoderSession` | initialization/metadata processing fails with non-empty error message |

### ALAC Decoder

Target files: `test/integration/audio/DecoderIntegrationTest.cpp`, optionally `test/unit/audio/AlacDecoderSessionTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Keep | `Decoder Bit-Perfect Conversions` | preserve current ALAC 24-to-32 bit-perfect conversion check | `hires.m4a` opened once as 24-bit output and once as 32-bit output | 32-bit samples equal unpacked 24-bit samples shifted left by 8 |
| Keep | `ALAC Decoder Integrity` | preserve current positive metadata check | `hires.m4a` | metadata stays `96000/2/24` |
| Add | `[P0] AlacDecoderSession - Open error and AAC rejection matrix` | cover missing file and unsupported AAC-in-M4A container | missing file path; `basic_metadata.m4a`; `with_cover.m4a` | missing file returns error; AAC `.m4a` is rejected with a non-empty parse/init error |
| Add | `[P0] AlacDecoderSession - Seek behavior matrix` | lock pre-open error and desired time-based seek behavior | `seek(100)` before open; after open on `hires.m4a`, compare block after `seek(0)` vs block after `seek(100)` | pre-open seek fails with `Timescale is 0`; post-open seek changes playback position instead of resetting to sample 0 |
| Add | `[P1] AlacDecoderSession - EOF and unsupported conversion matrix` | cover repeated EOF and `NotSupported` conversion branch | read until EOS on `hires.m4a`; separate open with `requestedOutput.bitDepth=16` | repeated reads stay EOS; unsupported `24 -> 16` conversion returns `NotSupported` |

### Engine

Target file: `test/unit/audio/PlaybackEngineTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Keep | `Engine - Basic Orchestration` | preserve stop cleanup, backend error, and route-ready snapshot behavior | current mocked backend and dispatcher | existing assertions remain the baseline contract |
| Keep | `Engine - Backend Swapping` | preserve idle backend swap behavior | current two-backend setup | old backend is reset/stopped/closed and status reflects the new device/backend |
| Keep | `Engine - Graph Initialization` | preserve internal graph population | `basic_metadata.flac` with mocked backend | `rs-decoder`, `rs-engine`, and their connection remain present |
| Keep | `Engine - PipeWire shared mode keeps native sample rate` | preserve shared-mode bypass of device caps | `basic_metadata.flac`, PipeWire shared backend caps at 48 kHz only | backend still opens at native 44.1 kHz |
| Keep | `Engine - Unsupported backend sample rate fails without resampler` | preserve current hard-fail branch | `basic_metadata.flac`, exclusive backend caps only 48 kHz | transport becomes `Error`, message mentions missing resampler |
| Keep | `Engine - Graph Integrity` | preserve real engine + `NullBackend` graph smoke | `basic_metadata.flac` | decoder and engine nodes remain observable |
| Add | `[P0] Engine - Play failure matrix` | cover unsupported extension, decoder open failure, invalid decoder output format, and backend open failure | `basic_metadata.mp3`; `basic_metadata.m4a`; scripted decoder returning `{sampleRate=0}`; mocked backend returning `InitFailed` from `open()` | transport becomes `Error`; status text matches the user-visible failure source; backend-open failure clears the source |
| Add | `[P0] Engine - Format negotiation and decoder reopen matrix` | cover channel-remap rejection and successful decoder reopen for negotiated output | `surround_5_1.flac` against stereo-only caps; `hires.flac` against `sampleFormats={32/24}` exclusive caps | channel-remap case fails with `no channel remapper yet`; hi-res reopen case succeeds and backend format becomes `32/24` |
| Add | `[P0] Engine - Pause and resume matrix` | cover pause from `Playing`, resume when backend already started, resume when backend was not started yet, and resume on drained source | valid FLAC playback with mocked backend and controlled source consumption | pause changes transport to `Paused`; resume calls `resume()` or `start()` correctly; drained resume resets to idle |
| Add | `[P0] Engine - Seek matrix` | cover no-op without source, active seek success, paused seek preserving paused state, and seek failure | `seek(100)` before play; valid FLAC + mocked backend; scripted decoder whose `seek()` fails | no-op without source; successful seek restarts buffering/playback; paused seek returns to `Paused`; failure sets `Error` |
| Add | `[P0] Engine - Drain and callback matrix` | cover pending-drain gate and track-ended callback | read all bytes through `onReadPcm()`, call `isSourceDrained()`, then `onDrainComplete()`; separate `onDrainComplete()` without pending drain | no-pending path is ignored; pending path resets to idle, clears route, and fires track-ended callback once |
| Add | `[P1] Engine - Format-changed and source-error matrix` | cover graph update and async source failure handling | call `onFormatChanged()` with `32/24`; separate scripted streaming source error during active playback and after idle | engine node format updates in both route snapshot and public status; active source error sets `Error`; late idle error is ignored |

### Player

Target file: `test/unit/audio/PlayerTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Keep | `Player - Quality Analysis with FakeIt` | preserve current bit-perfect, lossy source, resampling, volume, mute, external-mixing, and lossless-padding scenarios | current base engine route and base system graph | current quality conclusions remain the baseline contract |
| Keep | `Player - Lifecycle and Stale Updates with FakeIt` | preserve stale-generation ignore behavior | current route snapshot with incremented generation | stale route updates remain ignored |
| Keep | `Player - Pending Output` | preserve delayed output restoration path | current provider mock plus late `onDevicesChanged` callback | pending output is restored when the device appears |
| Add | `[P0] Player - Readiness and play-ignore matrix` | cover `isReady()` contract and ignored play before output selection | player with provider added but no active output; then with active output; then with pending output | `play()` is ignored when not ready; `isReady()` only returns true when backend is selected and no pending output remains |
| Add | `[P0] Player - setOutput matrix` | cover already-active output, missing device pending path, and missing-provider no-op path | fake provider status/device lists plus explicit `setOutput()` calls | already-active output clears pending state and does nothing else; missing device becomes pending; missing provider leaves engine unchanged |
| Add | `[P0] Player - Route subscription lifecycle matrix` | cover anchor appearance, anchor disappearance, and anchor change within the same generation | fake provider capturing `subscribeGraph()` anchors; call `handleRouteChanged()` with `anchor-a`, empty anchor, then `anchor-b` | graph subscription is created, cleared, and recreated as anchors change |
| Add | `[P1] Player - Merged graph enrichment matrix` | cover missing system-node format and missing stream node bridge cases | base engine route plus system graph nodes without `optFormat`; separate graph without a `Stream` node | missing formats inherit engine format; engine-to-stream bridge is added only when a stream node exists |
| Add | `[P1] Player - Quality analysis extension matrix` | cover channel remap, integer-to-float lossless mapping, precision truncation, inactive links, and deduped external-source names | variants of the base system graph with channel changes, float sink format, lower bit depth, inactive links, duplicate external-source names | quality becomes `LinearIntervention` or `LosslessFloat` as appropriate; inactive links are ignored; tooltip names are deduplicated and sorted |

### PipeWire Shared Helpers

Target file: `test/unit/audio/backend/PipeWireSharedTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Add | `[P0] PipeWireShared - parseUintProperty accepts only full decimal strings` | cover null, empty, non-numeric, partial, and valid numeric text | `nullptr`, `""`, `"abc"`, `"12abc"`, `"42"` | invalid inputs return `nullopt`; `"42"` returns `42` |
| Add | `[P0] PipeWireShared - parseRawStreamFormat maps all supported SPA formats` | parameterized format mapping table | `S16_LE`, `S24_LE`, `S24_32_LE`, `S32_LE`, `F32_LE`, `F64_LE` pods with known rate/channel counts | returned `Format` matches expected bit depth, valid bits, float flag, sample rate, and channels |
| Add | `[P1] PipeWireShared - parseRawStreamFormat rejects null and unsupported pods` | cover negative paths | `nullptr`, malformed pod, unsupported format id | returns `nullopt` |

### PipeWire Monitor

Target files: `test/unit/audio/backend/PipeWireMonitorTest.cpp`, `test/integration/audio/backend/PipeWireProviderTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Keep | `PipeWireProvider - Integration with Real Daemon via API` | preserve dummy sink and duplex enumeration smoke tests | current real-daemon test with `support.null-audio-sink` | current device discovery assertions remain green |
| Add | `[P0] PipeWireMonitor - Capability parsing matrix` | cover `sampleFormatCapabilityFromSpaFormat()`, `collectIntValues()`, `collectIdValues()`, and `parseEnumFormat()` behavior | synthetic SPA pods for enum/range sample rates, channels, and formats with duplicates | sample rates, formats, and channel counts are collected and deduplicated correctly |
| Add | `[P0] PipeWireMonitor - Sink property merge matrix` | cover volume, mute, soft mute, channel volume, and soft volume parsing | synthetic `SPA_PARAM_Props` pods with unity and non-unity variants | sink props are merged correctly and later yield correct `volumeNotUnity` / `isMuted` flags |
| Add | `[P0] PipeWireMonitor - Reachability and node classification matrix` | cover graph building with active links only, external upstream sources, and synthetic stream fallback | synthetic node/link maps with one playback stream, one intermediary, one sink, and one extra upstream source | `populateGraph()` includes only active/reachable nodes, classifies node types correctly, and emits expected connections |
| Add | `[P1] PipeWireMonitor - enumerateSinks matrix` | cover display name fallback order and shared/exclusive entry generation | node records with `nodeNick`, `nodeName`, `objectPath`, optional capabilities | each sink produces shared and exclusive entries; exclusive entry carries capabilities; display names follow `nodeNick -> nodeName -> objectPath` |
| Add | `[P2] PipeWireMonitor - subscribeGraph real-daemon smoke` | optional integration confidence for graph-rooting behavior | existing real PipeWire daemon plus a known route anchor | callback receives a graph rooted at the subscribed stream id |

### PipeWire Provider

Target file: `test/unit/audio/backend/PipeWireProviderTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Add | `[P0] PipeWireProvider - subscribeDevices prepends System Default` | provider contract independent of the real daemon | fake/injected monitor returning 2 devices | callback receives `System Default` first, followed by monitor devices |
| Add | `[P1] PipeWireProvider - status exposes both profiles and monitor devices` | metadata contract | fake/injected monitor returning known devices | metadata includes shared + exclusive profiles and returned devices match monitor snapshot |
| Add | `[P1] PipeWireProvider - subscribeGraph forwards route anchor` | pass-through contract | fake/injected monitor capturing the anchor | provider forwards the exact anchor string and callback |

### ALSA Provider

Target file: `test/unit/audio/backend/AlsaProviderTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Add | `[P0] AlsaProvider - subscribeDevices immediately emits cached devices and unsubscribe removes callback` | provider subscription contract | injected cached device list, then simulated refresh | initial callback sees cache immediately; after unsubscribe it no longer fires |
| Add | `[P0] AlsaProvider - status exposes only Exclusive profile metadata` | metadata contract | provider with fake cache | backend id is `alsa`; only exclusive profile is advertised |
| Add | `[P1] AlsaProvider - createBackend returns AlsaExclusiveBackend` | backend factory contract | any ALSA device | returned backend reports `backendId=alsa` and `profileId=exclusive` |
| Add | `[P1] AlsaProvider - subscribeGraph returns simple stream-to-sink graph` | current graph contract | `routeAnchor="hw:0,0"` | callback receives `alsa-stream -> alsa-sink` graph with sink name/object path derived from anchor |
| Add | `[P2] AlsaProvider - enumeration builds plughw and raw hw variants` | optional coverage for real enumeration logic | thin ALSA/udev seam returning one hardware card/device | provider cache contains both `plughw:x,y` and `hw:x,y` variants |

### PipeWire Backend

Target file: `test/unit/audio/backend/PipeWireBackendTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Add | `[P0] PipeWireBackend - open failure matrix` | cover missing loop/context/core, stream creation failure, and connect failure | injected PipeWire seam returning null loop/context/core or failing `pw_stream_new` / `pw_stream_connect` | `open()` returns `InitFailed` with non-empty message |
| Add | `[P0] PipeWireBackend - Stream process matrix` | cover no buffer, null data, normal read, stride fallback, and drained flush behavior | fake stream buffers plus captured callbacks; backend format `44.1/16/2` | bytes are queued correctly, position callback advances by frames, drained path triggers flush |
| Add | `[P0] PipeWireBackend - Param and state callback matrix` | cover negotiated format update, backend error, and route-anchor-once behavior | valid `SPA_PARAM_Format` pod; error state; paused/streaming states with a fixed node id | `onFormatChanged` fires with parsed format, `onBackendError` fires on error, `onRouteReady` fires once |
| Add | `[P1] PipeWireBackend - drain and flush idempotency matrix` | cover `_drainPending` behavior | repeated `drain()`, then `flush()`, then `stop()` | second `drain()` is ignored while pending; `flush()` and `stop()` clear pending state |
| Add | `[P1] PipeWireBackend - setExclusiveMode reopens only when a reopen is meaningful` | cover mode toggle branch | backend with and without explicit target device id | toggling mode with explicit target attempts reopen; toggling without target device only updates flag |

### ALSA Exclusive Backend

Target file: `test/unit/audio/backend/AlsaExclusiveBackendTest.cpp`

| Status | Test Name | Scenario | Test Data | Expected Result |
| --- | --- | --- | --- | --- |
| Add | `[P0] AlsaExclusiveBackend - open failure and fallback matrix` | cover `snd_pcm_open` failure, format rejection, and 16-bit-to-32-bit container fallback | injected ALSA seam returning open failure; separate seam rejecting `S16_LE` but accepting `S32_LE` | open failure returns `DeviceNotFound`; fallback succeeds and fires `onFormatChanged(bitDepth=32, validBits=16)` |
| Add | `[P0] AlsaExclusiveBackend - Route-ready callback is emitted on successful open` | current backend contract | successful open on fake device `hw:0,0` | `onRouteReady()` fires with `hw:0,0` |
| Add | `[P0] AlsaExclusiveBackend - Pause/resume matrix` | cover hardware pause supported and unsupported paths | one seam with `canPause=true`, one with `canPause=false` | supported path uses `snd_pcm_pause`; unsupported path uses `drop/prepare/start` |
| Add | `[P1] AlsaExclusiveBackend - Drain matrix` | cover drain with and without open PCM handle | `pcm=null`, then valid fake PCM | null-PCM path still invokes `onDrainComplete`; valid PCM drains and then invokes callback |
| Add | `[P1] AlsaExclusiveBackend - XRUN and suspend recovery matrix` | cover `-EPIPE` and `-ESTRPIPE` branches | fake playback loop encounters `snd_pcm_avail_update()` returning `-EPIPE` or `-ESTRPIPE` | underrun callback fires on `-EPIPE`; suspend branch retries resume until success |
| Add | `[P1] AlsaExclusiveBackend - Device lost matrix` | cover terminal backend error path | fake loop returns `-ENODEV` and `-EBADF` | `onBackendError()` fires with `ALSA Device Lost:` message |
| Add | `[P1] AlsaExclusiveBackend - Zero-byte read drains when source is exhausted` | cover end-of-playback branch in playback loop | `readPcm()` returns 0 and `isSourceDrained()` returns true | backend drains device and fires `onDrainComplete()` |

## Test Implementation Issues And Solutions

| Issue | Impact | Recommended solution |
| --- | --- | --- |
| Audio tests are inconsistently tagged | `Catch2` filters give a misleading picture of coverage | add `[audio]` plus layer/feature tags to every audio-related test case |
| `GraphAnalysisVerification.cpp` is a hardcoded-path debug test in the default binary | CI noise and false confidence | retag as `[manual][debug][audio]` and exclude from default automation |
| Audio tests reuse `TAG_TEST_DATA_DIR` | fixture ownership is confusing | add `AUDIO_TEST_DATA_DIR` and, if desired later, move audio fixtures under `test/integration/audio/test_data` |
| `Engine` hard-wires `createDecoderSession()` | decoder failure/reopen branches are awkward to unit-test | inject a `DecoderFactoryFn` or small dependency object into `Engine` |
| `Engine` chooses memory vs streaming source through anonymous helper logic | source-selection branch is hard to assert directly | either extract `shouldUseMemoryPcmSource()` into an internal helper or inject a `SourceFactory` in tests |
| `StreamingSource` is heavily threaded | naive sleep-based tests will be flaky | use latches/atomics, bounded waits, and scripted decoder hooks instead of long sleeps |
| `PipeWireMonitor` contains many anonymous pure helpers inside a 1305 LOC file | a lot of logic is only reachable through real-daemon integration | extract capability parsing, sink-prop parsing, and graph-building helpers into a small `detail` unit-testable surface |
| `PipeWireProvider` constructs a real monitor internally | provider behavior cannot be unit-tested without a daemon | add monitor injection or a small monitor factory seam |
| `PipeWireBackend` depends directly on `pw_stream_*` behavior | callback state-machine logic is currently untestable in isolation | add a very small internal stream seam instead of a full wrapper |
| `AlsaProvider` and `AlsaExclusiveBackend` depend directly on ALSA/udev C APIs | hardware and recovery paths cannot run in CI reliably | use a minimal `AlsaApi` seam for unit tests and keep at most one opt-in hardware smoke test |
| `basic_metadata.m4a` is AAC while `.m4a` is routed to `AlacDecoderSession` by extension | unsupported-codec behavior is currently implicit | keep an explicit negative test so the current product contract is intentional rather than accidental |

## Likely Defects Worth Locking With Tests

These are the highest-value "tests first, fix immediately after" items discovered during the audit:

1. `AlacDecoderSession::seek(positionMs)` ignores `positionMs` and always resets to sample 0.
2. `Player::handleRouteChanged()` does not obviously resubscribe if the route anchor changes within the same playback generation.
3. `parseUintProperty()` currently accepts partially numeric strings because it only checks `end == value`.
4. `GraphAnalysisVerification.cpp` is effectively a manual debug script but still lives in the default automation target.

## Recommended Execution Order

### Phase 1

Fast, hermetic, highest-value additions:

- `DecoderFactoryTest`
- `PcmRingBufferTest`
- `MemorySourceTest`
- `StreamingSourceTest`
- `PipeWireSharedTest`
- `FormatNegotiatorTest` expansion
- `PcmConverterTest` expansion

### Phase 2

State-machine expansion using existing mocks plus `ScriptedDecoderSession` and `CapturingBackend`:

- `PlaybackEngineTest` expansion
- `PlayerTest` expansion

### Phase 3

Codec/file edge cases:

- `DecoderIntegrationTest` expansion
- truncated FLAC / ALAC fixtures
- explicit AAC-in-M4A rejection checks

### Phase 4

PipeWire logic without daemon dependency:

- extract/test `PipeWireMonitor` helpers
- add unit tests for `PipeWireProvider`
- add seam-backed unit tests for `PipeWireBackend`

### Phase 5

ALSA logic with a thin seam:

- `AlsaProviderTest`
- `AlsaExclusiveBackendTest`

### Phase 6

Keep only a very small number of environment-dependent smoke tests:

- real PipeWire provider/monitor smoke
- optional ALSA hardware smoke if CI or developer machines can support it

## Expected Outcome

If Phases 1 through 4 are completed, the repository should move from "a few happy-path playback checks" to broad behavioral coverage across:

- format negotiation
- PCM conversion helpers
- decoder factory routing
- in-memory and threaded PCM sources
- FLAC and ALAC positive + negative behavior
- engine transport/error/drain/seek state transitions
- player graph-merging and quality analysis
- PipeWire parsing and provider/backend contracts

That is enough to catch most regressions without requiring real audio hardware in the default test run.
