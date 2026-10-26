---
id: media.cue-sheet
---
# CUE sheet

## Scope

This reference defines the text CUE sheet Aobus accepts and how that text becomes sample-frame segments of one audio file.
Library scan policy, stored ranges, and YAML interchange belong to [library scan and audio identity](../../system/library/scan-and-identity.md), the [database reference](../library/storage/database.md), and the [YAML format](../library/format/yaml.md).
Playback of a stored range is outside this page.

Aobus reads sheets. It never writes or edits one.

## Code boundary

The parser and name matcher live in Core: [`Sheet.h`](../../../include/ao/media/cue/Sheet.h) and [`Sheet.cpp`](../../../lib/media/cue/Sheet.cpp).
They need UTF-8 validation only, not charset detection.
Scan and add-file binding consume that result from `app/runtime/library/CueBinding.cpp`.

## Encoding

A sheet must be scalar-valid UTF-8, with or without a leading UTF-8 BOM.
The BOM is stripped before parsing. A NUL byte rejects the sheet.
Any other encoding is unreadable. There is no charset detection: a wrong guess would import titles that a later sheet fix does not correct, because imported metadata is curated.
The rejection asks the user to save the sheet as UTF-8. An explicitly chosen legacy encoding is later work and needs no format change.

## Grammar

Lines end at LF, CRLF, or a lone CR. Commands are ASCII case-insensitive. A sheet must contain exactly one `FILE` before its `TRACK`s.
The `FILE` filename is syntax only: it is not retained and is not used to find audio. The type must be `BINARY`, `MOTOROLA`, `AIFF`, `WAVE`, `MP3`, or `FLAC`.
`TRACK` numbers are 1 through 99 and strictly increase. An audio track needs `INDEX 01`. Index numbers strictly increase from 00 through 99 within a track, and index positions must not decrease within a track.
`INDEX 01` positions strictly increase across the sheet's tracks.
A time is `mm:ss:ff` with `ss` and `ff` each two digits, `ss < 60`, and `ff < 75`. Arithmetic is overflow-checked.
Quoted text uses a closing quote as the token end; a backslash is literal, not an escape. An unknown directive rejects the sheet. The diagnostic names that directive with a bounded excerpt of the rejected token, not an unbounded copy of it. An excerpt spells each ASCII control byte as `\xNN`, so sheet text cannot rewrite a terminal or log line.
`PREGAP` and `POSTGAP` are accepted once per track and describe silence that is not in the file, so they move no boundary.
`TITLE`, `PERFORMER`, `SONGWRITER`, `REM GENRE`, `REM DATE`, `REM DISCNUMBER`, and `REM TOTALDISCS` are read. `ISRC`, `CATALOG`, `CDTEXTFILE`, and other `REM` comments are not imported.
Each of those has one value per scope: before the first `TRACK` for the sheet, and within each track. A repeat with the same value, quoted or not, is accepted; a repeat with a different value rejects the sheet rather than choosing a copy. An explicit empty value is kept; import treats it as absent.
`FLAGS` is accepted and then discarded only after it appears on a `TRACK` and each flag token is `DCP`, `4CH`, `PRE`, or `SCMS`, using the same ASCII case-insensitive matching as other commands.
A `FLAGS` command with no flag, a flag outside that set, or `FLAGS` before any `TRACK` rejects the sheet.
Discarding `FLAGS` is not acceptance of arbitrary invalid syntax.
A sheet with no audio track is malformed. One audio track is valid and does not divide a file.
A data track (`CDG`, `MODE1/2048`, `MODE1/2352`, `MODE2/2048`, `MODE2/2324`, `MODE2/2336`, `MODE2/2352`, `CDI/2336`, or `CDI/2352`) creates no segment and does not by itself make the sheet malformed.

## Segments

Positions in the sheet are CD frames, 1/75 second. Resolved positions are PCM frames at the carrier's sample rate.
Rates 44100, 48000, 88200, 96000, 176400, and 192000 Hz convert exactly; every other positive rate rounds down.
A segment starts at `INDEX 01` of an audio track. Its end is the immediate successor's boundary, or the carrier's exact frame count when that track has no successor.
An immediate audio successor ends the segment at that track's `INDEX 01`, so its pregap plays at the end of the previous segment. Hidden track-one audio is not imported.
An immediate data successor ends the segment at that track's `INDEX 00` when the index exists, otherwise at its `INDEX 01`, clamped to the carrier end. A data successor with neither index rejects the layout. The data region is not a segment.
A later audio track starts at its own `INDEX 01`. An audio/data/audio sheet therefore keeps both audio ranges and excludes the data region between them. A data track that is not the immediate successor does not move an earlier audio boundary.
A start at or past the end, an empty range, or an overlap rejects the layout.
The segment's ordinal is the CUE `TRACK` number. It identifies the segment across rescans and is not the curated track number.

## Which file a sheet names

Matching uses already URI-normalized sibling names. Stems compare exactly. Extensions, including `cue`, compare ASCII case-insensitively, as audio-file recognition does.
`X.cue` names every supported file `X.<ext>` in its directory. `X.<ext>.cue` names only `X.<ext>` and takes that file over from `X.cue`.
On a case-sensitive file system, `X.flac.cue` and `X.FLAC.cue` are two candidates for `X.flac`. The nonempty set with the higher precedence decides; more than one sheet in that set is ambiguous and names no single sheet.
The `FILE` line is not resolved, including for an embedded sheet, because a rip often keeps a `.wav` name after conversion.

## Embedded text

Shared Vorbis-comment extraction may surface a `CUESHEET` comment from FLAC or Opus as embedded text. That callback is not a library text field.
Only a nonempty FLAC comment is an embedded binding source. It is the same single-`FILE` text and applies to its own file only when that file has no same-name external sheet.
An empty FLAC comment does not bind. Opus text does not bind. Identical nonempty comments count as one sheet.
Two different nonempty FLAC comments compete like same-name external sheets: neither binds, the file is undetermined, and the scan reports the ambiguity. A same-name external candidate blocks the FLAC fallback even when that candidate is unreadable, empty, outside the library root, or over the external read limit. There is no fallback to the embedded sheet or to importing the file whole. [CUE binding](../../system/library/scan-and-identity.md#cue-binding) owns resolution, in-root symlink containment, and that limit.
Several `FILE` commands make a parsed embedded sheet malformed. The binary FLAC `CUESHEET` metadata block, which carries offsets without titles, is not interpreted.

## Carrier files

Only FLAC and WAV can be divided, and only when the reader reports an exact PCM frame count: FLAC `STREAMINFO` total samples, where zero means unknown, or WAV data length divided by block alignment.
Any other codec, or a file without an exact extent, is not a carrier. Its dividing sheet is reported so the user can remove or rename the sheet and import the file whole.
A scan takes that report from the physical file's media reader, not from a stored member's codec or rate, even when the carrier is unchanged and every member records the same codec.
That is not a requirement that members share a codec, and it does not change the stored format.
[Scan and identity](../../system/library/scan-and-identity.md#cue-binding) owns when the file is opened.
Open admission and a full YAML restore do not re-check the codec by decoding the file.
Full restore trusts the payload's structural facts, as [YAML transfer](../../system/library/yaml-transfer.md#carrier-tracks) specifies.
A later build that cannot use a stored carrier's codec leaves that carrier unchanged and treats its sheet as undetermined.

## Implementation authority

- [`Sheet.h`](../../../include/ao/media/cue/Sheet.h) declares the sheet, the resolved segment, and the same-name candidate helpers.
- [`Sheet.cpp`](../../../lib/media/cue/Sheet.cpp) owns UTF-8 admission, the grammar, and range resolution.
- [`Visitor.h`](../../../include/ao/media/file/Visitor.h) exposes embedded sheet text through `cueSheet`, not a text field. Shared Vorbis-comment extraction copies a `CUESHEET` comment for FLAC and Opus; the FLAC reader skips the binary cue-sheet block.

## Test authority

- [`CueSheetTest.cpp`](../../../test/media/CueSheetTest.cpp) locks the grammar, data-track ends, pregap placement, UTF-8 rejection, and same-name candidate sets.
- [`ExactExtentTest.cpp`](../../../test/media/ExactExtentTest.cpp) locks exact FLAC and WAV extents and that a binary `CUESHEET` block stays uninterpreted while comment text is copied.

## Related documents

- [Supported audio files](audio-file.md)
- [Library scan and audio identity](../../system/library/scan-and-identity.md)
- [Track model](../library/model/track.md)
- [Manage a music library](../../user/manage-library.md)
