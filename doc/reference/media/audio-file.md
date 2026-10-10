---
id: media.audio-file-library-surface
---
# Supported audio files

## Scope and version

This reference defines the exact audio-file surface consumed by library scanning, initial metadata import, and audio identity.
Behavior belongs to the [media file reading specification](../../system/media/file-reading.md); PCM behavior belongs to the [decoder session specification](../../system/playback/decoder-session.md).

## Code boundary

The public Core surface is `ao::media::file::File`, `Visitor`, and `PayloadView` under `include/ao/media/file/`. Format readers live under `lib/media/file/`; both are part of the `ao_media` target. The [encoded media architecture](../../system/media/README.md) owns the target boundary and borrowed-data lifetimes, the [system architecture](../../system/overview.md) owns top-level dependency direction, and application runtime owns the one visitor-to-library adapter described by [library architecture](../../system/library/structure.md).

## Supported extensions and codecs

`File::isSupported()` and `File::open()` share one case-normalized extension table.

| Extension | Reader/container | `AudioCodec` emitted |
|---|---|---|
| `.flac` | FLAC | `Flac` |
| `.mp3` | MPEG audio with optional ID3 | `Mp3` |
| `.m4a` | MP4 audio | `Alac` for `alac`; `Aac` for `mp4a`; otherwise no codec callback |
| `.wav` | RIFF/WAVE | `Wav` |
| `.opus` | Ogg Opus | `Opus` |

Other paths are unsupported. In particular, `.mp4`, ADTS `.aac`, literal `.alac`, images, and playlists are not recognized scan inputs. `.m4a` is the only accepted MP4 audio extension. `.ogg` is also unrecognized: it names a container rather than a codec, and its common Vorbis payload has no reader, so accepting it would make scanning report errors for most real files. Opus is recognized only under its own `.opus` extension.

## Visitor surface and order

A successful visit emits present values in this fixed order:

1. `TextField`: `Title`, `Artist`, `Album`, `AlbumArtist`, `Composer`, `Genre`, `Work`, `Movement`.
2. `NumberField`: `Year`, `TrackNumber`, `TrackTotal`, `DiscNumber`, `DiscTotal`, `MovementNumber`, `MovementTotal`.
3. Technical callbacks: `codec`, `duration`, `bitrate`, `sampleRate`, `channels`, `bitDepth`.
4. `picture` callbacks in source order.
5. One `visitCredits` callback containing the complete source-ordered name/kind/optional-role list, only when nonempty.

Empty text, zero numeric and technical values, `AudioCodec::Unknown`, and rejected pictures produce no callback. A required failure produces no callback at all.
The Credits callback is synchronous, follows every scalar, technical, and picture callback, and is emitted at most once per visited content.
Its span and entry strings borrow the content owner's storage for the backing `File` lifetime; the default visitor implementation ignores it.
Consumers needing longer-lived facts must copy owning values before that backing expires.
The callback preserves source traversal order and duplicates; library persistence later stably groups entries into canonical kinds.
Visitor text has undergone the source format's declared legacy-encoding conversion where applicable, but this media surface does not promise scalar-valid UTF-8.
`readMediaTrack` passes the bytes to core library admission, where malformed text rejects that scan item and valid text is normalized to NFC before persistence; no generic replacement-character repair occurs between the two boundaries.

## Source field mapping

Mappings are case-insensitive for FLAC/Vorbis keys, MP4 metadata/freeform aliases where stated, and ID3 `TXXX` keys. A dash means no mapping.

Opus carries its metadata as a Vorbis comment list in its `OpusTags` packet and therefore uses the FLAC/Vorbis column below unchanged, including whole-file `ORCHESTRA` fallback and repeated `PERFORMER` credits.
Conductor, Ensemble, Soloist, and Performer rows below emit credit kinds, not scalar `TextField` callbacks. WAVE embedded ID3 uses every supported ID3 mapping, not just `TMCL`; `INFO` has no credit mapping.

| Visitor field | FLAC/Vorbis | MP4/iTunes or `mdta` | ID3v2 | WAVE `INFO` |
|---|---|---|---|---|
| `Title` | `TITLE` | `©nam`; `title` | `TIT2` | `INAM` |
| `Artist` | `ARTIST` | `©ART`; `artist` | `TPE1` | `IART` |
| `Album` | `ALBUM` | `©alb`; `album` | `TALB` | `IPRD` |
| `AlbumArtist` | `ALBUMARTIST`, `ALBUM ARTIST`, `ALBUM_ARTIST` | `aART`; `album_artist`, `albumartist` | `TPE2` | — |
| `Composer` | `COMPOSER` | `©wrt`; `composer` | `TCOM` | — |
| `Conductor` | `CONDUCTOR` | freeform/`mdta` `conductor` | `TPE3`; `TXXX:conductor` | — |
| `Ensemble` | `ENSEMBLE`; fallback `ORCHESTRA` | freeform/`mdta` `ensemble`; fallback `orchestra` | `TXXX:ensemble`; fallback `TXXX:orchestra` | — |
| `Genre` | `GENRE` | `©gen`; `genre` | `TCON` | `IGNR` |
| `Work` | `WORK` > `GROUPING` | `©wrk` > `mdta` `work` > `©grp` > `mdta` `grouping` | `TXXX:work` > `TIT1` > `TXXX:grouping` | — |
| `Movement` | `MOVEMENTNAME` | `©mvn`; `movementname`, `movement_name`, `mvnm` | `MVNM`; equivalent `TXXX` aliases | — |
| `Soloist` | `SOLOIST` | freeform/`mdta` `soloist` | `TXXX:soloist` | — |
| Performer credits | Repeated `PERFORMER` | — | ID3v2.4 `TMCL` | Embedded ID3, not `INFO` |
| `Year` | `DATE`, `YEAR` | `©day`; `date`, `year` | `TYER`, `TDRC` | first four decimal digits of `ICRD` |
| Track number/total | `TRACKNUMBER`, `TRACK` as `n` or `n/total`; `TRACKTOTAL`, `TOTALTRACKS` | `trkn`; `track`, `tracknumber` slash value | `TRCK` slash value | — |
| Disc number/total | `DISCNUMBER`, `DISC` slash value; `DISCTOTAL`, `TOTALDISCS` | `disk`; `disc`, `disk`, `discnumber` slash value | `TPOS` slash value | — |
| Movement number/total | `MOVEMENT` slash value; `MOVEMENTTOTAL` | `©mvi`, `©mvc` signed non-negative big-endian integer; equivalent `mdta` aliases | `MVIN` slash value; equivalent `TXXX` aliases | — |

Unknown fields do not become `TrackBuilder::customMetadata`. Invalid isolated scalar values leave their field at zero.
Supported credit values accumulate rather than overwrite, including repeated values and different mapped source forms for one kind. There is no deduplication.
Ensemble precedence is whole-file and nonblank: any explicit Ensemble candidate with a nonblank ASCII-trimmed name retains all nonblank explicit candidates and discards all Orchestra candidates, in either traversal order.
Without such an explicit candidate, all nonblank Orchestra candidates become Ensemble entries in source order.
A blank explicit tag neither contributes an entry nor suppresses fallback. Explicit `SOLOIST` selects Soloist; `PERFORMER` always selects Performer.

Work sources use the strictly ordered precedence shown above, independent of tag traversal order.
A candidate containing only surrounding ASCII whitespace (space, tab, carriage return, line feed, form feed, or vertical tab) is absent and cannot suppress a lower-priority source.
Nonempty text retains its original whitespace; this rule does not change scalar admission or repair invalid encodings.
Repeated nonempty values from the same source keep the last value in traversal order.
These chains are Aobus compatibility policy: grouping is a fallback, and ID3 `TIT1` remains an ambiguous content-group frame rather than universally meaning a musical work.
MP4 `mdta` aliases remain supported; freeform work/grouping and ID3 `GRP1` are not added.

New imports use this work precedence.
An ordinary changed-file rescan preserves the stored work, whether file-derived or manually curated, and therefore does not correct older values; see [scan application](../../system/library/scan-and-identity.md#plan-application).

MP4 textual atoms, `mdta` fields, and supported freeform `data` children accept only declared UTF-8: data type `1`, version `0`.
UTF-16, binary, type `0`, and nonzero-version textual values are omitted, not decoded or reinterpreted as UTF-8.
Declared UTF-8 bytes are passed through for normal library text admission; this declaration does not repair malformed text.

MP4 integer movement payloads accept widths 1, 2, 3, 4, or 8, data type `0` or `21`, version `0`, and values from `0` through `65535`. Negative, wider, unsupported-type, unsupported-version, and overflowing values are ignored.

## Credit import

Credits carry supplied participants with a fixed kind and optional descriptive role/instrument text; they need not be a complete personnel roster.
They are the stored authority for Conductor, Ensemble, Soloist, and Performer, while Composer, Artist, and AlbumArtist remain separate.
Source tags select initial kinds; roles never classify entries, and curated kinds do not retain source provenance.
The public callback and runtime adapter carry the complete list into `MetadataBuilder::credits` through an exhaustive explicit media-to-library kind mapping.
The media layer has its own small kind/view types and does not depend on library storage.
Core applies the [credit input gate](../library/model/track.md#metadata-input-values) before persistence.
Media skips blank source names, unlike explicitly authored editor/CLI/YAML entries, whose blank names reject the replacement.
Invalid nonblank text fails library admission rather than receiving replacement-character repair or silent fallback.

ID3 TPE3 and supported TXXX credit mappings reuse version-aware text decoding: ID3v2.4 NUL-separated values contribute separate entries; older-version decoding stays unchanged.
Neither commas nor slashes split these fields. Supported MP4 freeform and `mdta` mappings retain their existing keys; no new metadata scheme is inferred.

ID3v2.4 `TMCL` consumes alternating instrument and musician strings after decoding.
Empty decoded positions must remain in place before pairing.
Split only the musician position on commas, trim the six ASCII whitespace characters from each resulting name and the instrument text, and emit each nonempty name with that pair's optional role.
An empty instrument means no role, an empty musician position contributes no entry, and an unpaired final instrument is ignored without discarding earlier complete pairs.
Repeated frames retain entry order and duplicates.
ID3v2.3 `TMCL`, `TIPL`, and other contributor schemes are not interpreted as credit sources.
Each emitted TMCL entry is Performer, with instrument as role. A comma inside one person's name remains ambiguous; no surname heuristic repairs it.
This comma splitting is not applied to `ARTIST`, `SOLOIST`, TPE3/TXXX, or Vorbis `PERFORMER`.

For each Vorbis `PERFORMER`, trim surrounding ASCII whitespace and omit a blank value.
Recognize only a nonempty name followed by one final `(role)` suffix separated by one or more ASCII whitespace characters, with a nonempty trimmed role and no other or nested parentheses.
Consume the whole separating whitespace run and trim both parts: `John Smith  (Piano)` becomes name `John Smith`, role `Piano`.
Otherwise retain the complete trimmed value as an unstructured name with absent role.
Commas and semicolons are not separators, and repeated comments remain separate ordered entries.
This is conservative support for the Picard convention, not verified identity or proof that a parenthesized suffix is an instrument.

`PERFORMER` never implies Soloist. New scans populate Credits; ordinary rescans preserve complete curated credits, including empty segments, roles, order, and duplicates.

WAVE accumulates every credit kind from successive valid `id3 ` or `ID3 ` chunks in RIFF traversal order, retaining duplicates across chunks as well as within frames.
It copies every name and role into the WAVE content owner before the temporary embedded-ID3 builder dies, including converted and unsynchronised text.
Explicit Ensemble and Orchestra candidates remain separate until whole-file precedence resolves, so an explicit candidate in either an earlier or later chunk wins over all fallback candidates.
A malformed optional chunk contributes no entries and does not erase credits already accepted from earlier chunks.
The final visitor emits the accumulated list once, not one replacement callback per chunk.

RecordingDate has no automatic file-tag mapping or media callback.
It is never inferred from `DATE`, `YEAR`, ID3 `TDRC`, the hot Year, or album/work text; manual and portable authoring remain independent of generic Year import.

## Cover import

`PictureType` uses the ID3v2/FLAC numeric range `0x00` through `0x14` (`Other` through `PublisherLogo`). FLAC picture blocks and ID3v2 APIC frames preserve their source role and order; out-of-range roles normalize to `Other`. MP4 `covr` images preserve source order and emit `FrontCover` because that container entry has no APIC-style role. Opus covers arrive as Base64 `METADATA_BLOCK_PICTURE` comments whose decoded body has the same layout as a FLAC picture block and therefore preserve their source role; a comment that is not valid Base64 or whose body does not exactly fill that layout contributes no cover. The legacy `COVERART` comment is not read. WAVE `INFO` contributes no cover callback; valid embedded-ID3 APIC frames do, preserving their roles and source order.
WAVE retains copied bytes when ID3 conversion or unsynchronisation used the temporary chunk owner's storage.

Image bytes are borrowed views at this boundary. `readMediaTrack` passes them to the track cover builder, and `ResourceStore` later records a descriptor naming that content by digest; the bytes themselves stay in the backing file mapping or, where a container stores them encoded as with Opus Base64 comments, in the interpreted-content cache that decoded them, and identical content deduplicates to one row.

## Technical properties

| Format | Property sources |
|---|---|
| FLAC | `StreamInfo` sample rate, channels, bit depth, and total-sample duration; bitrate from mapped file bytes and duration. |
| MP3 | First confirmed MPEG frame; table bitrate or adjacent-frame-derived free-format bitrate; Xing frame/byte evidence when valid, otherwise frame bitrate and tag-trimmed payload extent. Bit depth is `16`. |
| MP4 | Selected audio track `mdhd` timing and first `stsd` `alac`/`mp4a` sample entry. Malformed optional timing may leave duration absent. |
| WAVE | Validated format and data chunks; duration from frames and sample rate, bitrate from mapped file bytes and duration. |
| Opus | `OpusHead` channel count; sample rate is always the fixed 48 kHz decode rate, never the header's informational input rate. Duration is the final page granule position less the stream's playback start, which is the derived decode origin advanced past the header pre-skip; bitrate comes from mapped file bytes and duration. A stream that never reached a complete end of stream reports no duration and therefore no bitrate. No bit depth is emitted, because Opus defines none. |

## Encoded audio payload

`File::audioPayload()` returns a borrowed non-empty span plus its file offset:

| Format | Payload range |
|---|---|
| FLAC | All bytes after the final metadata block. |
| MP4 | Payload of the single non-empty top-level `mdat`, after its compact or extended header; a size `0` `mdat` extends to end of file. Multiple `mdat` atoms are rejected. |
| MP3 | From the first confirmed MPEG frame through the bytes before validated trailing ID3v1/APEv2. A valid leading ID3v2 envelope is excluded. |
| WAV | RIFF `data` chunk payload. |
| Opus | From the start of the first audio page through the end of the file, excluding the identification and tag pages. |

The range excludes known tag and non-audio container regions but does not decode samples. MP3 encoder padding inside frames remains part of the payload. Opus page headers stay inside the range, so a tag edit that changes how many pages the Opus header occupies shifts the audio page sequence numbers and changes the identity of an otherwise unmodified file.

## Errors and lifetime

| Condition | Result |
|---|---|
| Unsupported extension | `NotSupported` from `File::open()` |
| Mapping failure | `IoError` with path context |
| Recognized WAVE with an unsupported format code, extensible subformat, channel count, valid-bit/container combination, PCM bit depth, or float representation | `NotSupported` from `visit()` or `audioPayload()` |
| Required malformed container or empty/ambiguous payload | `CorruptData` or `FormatRejected` |
| Bounded malformed optional metadata subtree | Successful omission |

`File` is move-only and non-concurrent. Payload, text, and image views remain valid while the backing `File` lives; moving the file transfers that backing without copying it. `MediaTrack` is the runtime owner that pairs such views with a library builder.

## Compatibility and versioning

Adding an extension requires the single dispatch entry plus parser, scan, payload, and identity tests. Adding a second Ogg codec additionally requires deciding how `.ogg` selects a reader, because that extension does not name one. Changing a field mapping requires this reference and matching fixtures to change together. Changing an encoded payload boundary changes persisted identity semantics and requires a database version increment or an explicitly specified compatible re-index policy.

## Implementation authority

- [`File.h`](../../../include/ao/media/file/File.h) and [`Visitor.h`](../../../include/ao/media/file/Visitor.h) define the public surface.
- Format readers under [`lib/media/file/`](../../../lib/media/file) own source mapping and payload boundaries.
- [`PictureType.h`](../../../include/ao/PictureType.h) owns cover-role values.
- [`AudioCodec.h`](../../../include/ao/AudioCodec.h) owns codec values.

## Test authority

- Format tests under [`test/unit/media/file/`](../../../test/unit/media/file) lock exact fields, covers, codecs, payloads, errors, and lifetime behavior.
- [`FileTest.cpp`](../../../test/integration/media/file/FileTest.cpp) locks real fixtures.
- [`Mp4FileTest.cpp`](../../../test/unit/media/file/Mp4FileTest.cpp) locks declared text type/version admission, omission of unsupported text forms, and preservation of declared UTF-8 bytes for library admission.
- [`ScanPlanTest.cpp`](../../../test/unit/runtime/library/ScanPlanTest.cpp) locks recognition.
- [`AudioIdentityTest.cpp`](../../../test/unit/library/AudioIdentityTest.cpp) locks payload hashing.
- [`CreditReaderTest.cpp`](../../../test/unit/media/file/id3v2/CreditReaderTest.cpp), [`VorbisCreditTest.cpp`](../../../test/unit/media/file/VorbisCreditTest.cpp), and [`WavCreditContentTest.cpp`](../../../test/unit/media/file/WavCreditContentTest.cpp) specify positional extraction, conservative suffix handling, cross-chunk order/duplicates, and backing lifetime.
- [`WavFileTest.cpp`](../../../test/unit/media/file/WavFileTest.cpp) includes embedded APIC and unsynchronised-content cases.

## Related documents

- [System architecture](../../system/overview.md)
- [Encoded media architecture](../../system/media/README.md)
- [Library architecture](../../system/library/structure.md)
- [Media file reading specification](../../system/media/file-reading.md)
- [Track model](../library/model/track.md)
- [Library scan and audio identity](../../system/library/scan-and-identity.md)
- [Decoder session](../../system/playback/decoder-session.md)
