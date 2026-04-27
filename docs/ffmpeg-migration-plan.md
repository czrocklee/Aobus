## FFmpeg Migration Plan

Date: 2026-04-26

## Goal

Replace FFmpeg in RockStudio playback with:

- `libFLAC` for FLAC playback
- `alac` (`macosforge/alac`) for ALAC decoding

This document summarizes the findings from the initial investigation and lays out the implementation plan before the decoder swap proceeds.

## Current Findings

### 1. FFmpeg is only used directly in playback

The runtime playback decoder is currently the concrete `FfmpegDecoderSession` implementation.

Relevant files:

- `app/core/playback/FfmpegDecoderSession.cpp`
- `app/core/playback/FfmpegDecoderSession.h`
- `app/core/playback/PlaybackEngine.cpp`
- `app/core/playback/MemoryPcmSource.h`
- `app/core/playback/StreamingPcmSource.h`

The rest of the playback pipeline is already split reasonably well into:

- decoder
- PCM source
- audio backend

That means the decoder backend can be replaced without redesigning the whole playback engine.

### 2. A backend-neutral decoder seam is already in the working tree

A decoder abstraction was already introduced so the rest of playback no longer needs to depend on the concrete FFmpeg type.

Relevant files:

- `app/core/decoder/IAudioDecoderSession.h`
- `app/core/decoder/AudioDecoderFactory.cpp`
- `app/core/Log.cpp`

This refactor already builds and passed the full test binary. It is intended to be the seam used for the FLAC and ALAC replacements.

### 3. FFmpeg is still present in build wiring and helper scripts

The codebase still references FFmpeg in:

- `CMakeLists.txt`
- `app/CMakeLists.txt`
- `test/CMakeLists.txt`
- `shell.nix`
- `tool/generate_test_library.sh`
- `test/integration/tag/generate_test_files.sh`
- `test/integration/tool/test_rsc.sh`

This means the runtime migration and the repo-wide FFmpeg cleanup are related, but not exactly the same task.

### 4. The existing MP4 parser is useful for ALAC playback

RockStudio already has an MP4 atom parser used for metadata and audio property extraction.

Relevant files:

- `lib/tag/mp4/File.cpp`
- `lib/tag/mp4/Atom.cpp`
- `lib/tag/mp4/Atom.h`
- `lib/tag/mp4/AtomLayout.h`

It currently parses enough structure to read:

- `mdhd`
- `stsd`
- metadata atoms under `ilst`

It does not yet parse packet tables needed for playback demux:

- `stsc`
- `stsz`
- `stco`
- `co64`

But it provides a good base for an in-tree ALAC demuxer.

### 5. `libFLAC` is a good replacement for FLAC playback

The `libFLAC` decoder API supports:

- file-based decoding
- metadata callbacks
- PCM write callbacks
- exact sample-based seeking

That makes it a clean fit for a `FlacDecoderSession` implementation.

### 6. `macosforge/alac` is decode-only, not a container parser

The ALAC library does not parse `.m4a` or `.mp4` containers.

It expects the caller to provide:

- the ALAC magic cookie extracted from the MP4 sample description (`stsd`)
- one raw ALAC packet at a time
- packet sizing and seek math derived from MP4 sample tables

In other words, linking `alac` alone is not enough. ALAC playback requires a small MP4 demux layer inside RockStudio.

### 7. Nixpkgs already provides both required libraries

Both libraries are available in nixpkgs and fit the existing `nix-shell` workflow:

- `alac`
- `flac`

That means the migration can stay reproducible inside the current environment instead of vendoring new third-party code.

### 8. Format coverage will shrink if FFmpeg is removed now

If FFmpeg is removed and playback is replaced only with `libFLAC` and `alac`, then playback support becomes:

- supported: FLAC
- supported: ALAC in `.m4a/.mp4`
- unsupported: MP3
- unsupported: AAC

This is a product decision as much as a technical one.

Tag import for MP3 and MP4 is separate from playback and is not blocked by this decoder migration.

### 9. Existing fixtures are good enough to validate the migration

The repository already contains committed test fixtures here:

- `test/integration/tag/test_data/basic_metadata.flac`
- `test/integration/tag/test_data/hires.flac`
- `test/integration/tag/test_data/basic_metadata.m4a`
- `test/integration/tag/test_data/hires.m4a`

The ALAC high-resolution fixture is especially useful because it exercises the intended ALAC path without relying on generated runtime fixtures.

### 10. Current playback tests are light smoke tests

The current playback decoder tests in `test/integration/playback/FfmpegDecoderSessionTest.cpp` do not deeply lock down behavior.

That is good news for the migration because the test surface is not huge, but it also means the replacement should add a few high-value regression tests around:

- opening FLAC
- opening ALAC
- reading decoded PCM
- seeking

## Assumptions

This plan assumes the intended immediate direction is:

1. Remove FFmpeg from playback.
2. Replace it with FLAC and ALAC playback only.
3. Accept that MP3 and AAC playback will no longer work for now.

If MP3 or AAC playback must remain supported, this plan is incomplete and another decoder strategy is needed.

## Detailed Plan

### Phase 1: Finish the abstraction transition

Update the decoder factory so selection is based on file path and format, not just output format.

Planned behavior:

- `.flac` -> `FlacDecoderSession`
- `.m4a` / `.mp4` -> `AlacDecoderSession`
- anything else -> clear unsupported-format error

Relevant files:

- `app/core/decoder/IAudioDecoderSession.h`
- `app/core/decoder/AudioDecoderFactory.cpp`
- `app/core/playback/PlaybackEngine.cpp`

### Phase 2: Replace FFmpeg in build configuration

Remove FFmpeg from the playback build and add the new decoder libraries.

Planned changes:

- remove FFmpeg pkg-config discovery from `CMakeLists.txt`
- add pkg-config discovery for `flac` and `alac`
- update `app/CMakeLists.txt` to compile new decoder sources
- update `test/CMakeLists.txt` to compile new decoder tests
- update `shell.nix` to add `flac` and `alac` and remove `ffmpeg` from the dev environment if a full migration is desired

Note:

The shell and helper-script cleanup can be done either in the same change or as a follow-up, depending on whether the goal is runtime playback migration only or full repo-wide FFmpeg removal.

### Phase 3: Implement `FlacDecoderSession`

Add a new `IAudioDecoderSession` implementation for FLAC.

Responsibilities:

- open `.flac` files with `libFLAC`
- collect stream metadata from `STREAMINFO`
- expose source/output `StreamFormat`
- decode frame-by-frame into interleaved `PcmBlock`
- support `seek(positionMs)` via sample-accurate FLAC seeking
- support `close()`, `flush()`, and error reporting compatible with the existing decoder interface

Notes:

- keep the implementation minimal and focused
- do not add extra format-conversion features unless required by the current playback pipeline
- prefer matching the existing `PcmBlock` contract exactly

### Phase 4: Add a minimal MP4 demux helper for ALAC

Create a focused helper for playback-only MP4 demux.

It should parse only the atoms needed for ALAC playback:

- `moov`
- `trak`
- `mdia`
- `minf`
- `stbl`
- `mdhd`
- `stsd`
- `stsc`
- `stsz`
- `stco`
- `co64`

Responsibilities:

- identify the audio track whose sample entry type is `alac`
- extract the ALAC magic cookie from `stsd`
- read duration and timescale from `mdhd`
- derive packet sizes from `stsz`
- derive chunk offsets from `stco` or `co64`
- map packets to chunks via `stsc`
- precompute enough offset information for sequential decode and seek

Important constraint:

This should be playback-focused, not a general MP4 framework.

### Phase 5: Implement `AlacDecoderSession`

Add a new `IAudioDecoderSession` implementation for ALAC-in-MP4.

Responsibilities:

- open `.m4a/.mp4`
- use the MP4 demux helper to locate the ALAC track and packet tables
- initialize `ALACDecoder` with the extracted magic cookie
- feed one raw ALAC packet at a time into the codec
- emit decoded PCM as `PcmBlock`
- support time-based seek by mapping the requested position to packet index and frame offset
- reject non-ALAC `.m4a/.mp4` files with a clear error

Important limitation:

This decoder should explicitly fail on AAC MP4 files rather than silently pretending generic MP4 playback is supported.

### Phase 6: Remove FFmpeg playback code

Once FLAC and ALAC paths are in place:

- stop compiling `app/core/playback/FfmpegDecoderSession.cpp`
- remove `FfmpegDecoderSession` build guards from app/test playback wiring
- remove FFmpeg-specific playback initialization code that is no longer needed

Depending on the desired cleanup scope, this phase may also include:

- deleting the FFmpeg decoder source files
- removing FFmpeg mentions from developer shell and helper scripts

### Phase 7: Replace or rewrite playback tests

Retire the FFmpeg-specific playback test and replace it with backend-neutral decoder tests.

Suggested tests:

- open `hires.flac` and verify stream info
- decode at least one block from `hires.flac`
- open `hires.m4a` and verify stream info
- decode at least one block from `hires.m4a`
- seek on one FLAC and one ALAC file and verify decoding resumes successfully

Keep the test count small and focused on behavioral boundaries.

### Phase 8: Verify end-to-end

Validation steps after implementation:

1. `nix-shell --run "cmake --preset linux-debug"`
2. `nix-shell --run "cmake --build /tmp/build --parallel"`
3. `nix-shell --run "/tmp/build/test/rs_test"`

If the repo-wide FFmpeg cleanup is included, also verify helper scripts or regenerate fixtures as needed.

## Risks And Tradeoffs

### Main technical risk: MP4 packet-table handling

The hardest part of this migration is not ALAC decoding itself.

The real risk is implementing the MP4 packet-table logic correctly:

- sample-to-chunk mapping
- packet offset calculation
- seek-to-packet mapping
- partial packet trimming after seek

That part needs careful implementation and a small number of strong tests.

### Main product tradeoff: reduced playback coverage

Removing FFmpeg in this phase means giving up broad codec support.

After this migration, RockStudio playback would be intentionally focused on:

- FLAC
- ALAC

That is a clean technical direction, but it is still a visible product change.

### Secondary cleanup question: scripts vs runtime

There are two different scopes available:

1. Runtime migration only
2. Full repo-wide FFmpeg removal, including fixture-generation scripts

The runtime path is the priority. The script cleanup can follow if needed.

## Current Workspace State

Snapshots created before the larger edits:

- `rockstudio-decoder-abstraction-20260426-000918`
- `rockstudio-flac-alac-20260426-002305`

The decoder abstraction refactor is already present in the working tree and passed the full test binary before the backend replacement started.

The FLAC/ALAC backend replacement itself has not been started yet.

## Recommendation

Proceed with the migration only if the team agrees on this immediate target state:

- keep playback for FLAC
- keep playback for ALAC in MP4/M4A
- drop FFmpeg-based MP3/AAC playback for now

If that is accepted, the plan above is the smallest coherent path away from FFmpeg without introducing GStreamer or another broad media framework.
