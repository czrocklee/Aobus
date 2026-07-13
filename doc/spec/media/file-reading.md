---
id: media.file-reading
type: spec
status: current
domain: media
summary: Defines recognition, lazy read behavior, visitor delivery, payload extraction, malformed-input containment, and view lifetimes for supported audio files.
---
# Media file reading

## Scope

This specification defines current behavior for recognizing and opening supported encoded audio files, visiting normalized metadata and technical properties, extracting covers, and selecting encoded-audio payload ranges.

The [supported audio files reference](../../reference/media/audio-file.md) owns the exact extension, field, codec, cover-role, callback-order, and payload-range inventories. Library scan reconciliation and identity publication belong to [library scan and audio identity](../library/runtime/scan-and-identity.md); PCM behavior belongs to the [decoder session](../playback/decoder-session.md).

## Code boundary

This contract belongs to the `media::file` sub-boundary of the Core `ao_media` target. Its public boundary is `include/ao/media/file/`; format implementations are private under `lib/media/file/` and may consume the target's reusable container parsers. The [encoded media architecture](../../architecture/encoded-media.md) owns the target's internal dependency and lifetime model, the [system architecture](../../architecture/system-overview.md) owns top-level layering, and [library architecture](../../architecture/library.md) owns the ingestion endpoint.

The private application-runtime `readMediaTrack` adapter converts visitor calls into a `library::TrackBuilder` and returns a `MediaTrack` that retains the backing file. Parser code does not include that builder or any library, runtime, UIModel, or frontend type.

## Terminology

- **Supported path**: a path whose case-normalized extension occurs in the single `File` dispatch table.
- **Required evidence**: structure necessary to select a usable non-empty encoded payload and safely identify the format.
- **Optional evidence**: metadata, artwork, or technical enrichment whose bounded failure does not make the encoded payload unusable.
- **Payload view**: a non-owning `PayloadView` containing encoded bytes and their offset in the mapped file.
- **Visitor view**: a string or byte view emitted during `visit()` and backed by the mapped file or the file's interpreted-content cache.

## Invariants

- `File::isSupported()` and `File::open()` derive recognition from the same extension dispatch.
- `open()` validates extension and read-only mapping, but parsing remains lazy.
- `visit()` succeeds only when required parsing and non-empty payload selection succeed.
- A failed `visit()` invokes no visitor callback.
- A successful visit emits only accepted non-empty text, non-zero numeric and technical values, a known codec, and accepted non-empty pictures.
- `audioPayload().offset` and `bytes.size()` describe a range entirely inside the mapped file.
- Required parse success or failure is cached and reused by payload and content operations.
- Returned views remain valid while the backing `File` lives, including after moving that `File`.
- One `File` instance is not safe for concurrent calls.

## State model

`File` owns one mapped file and one selected format reader. It is move-only. Its logical state advances lazily:

```text
mapped
  -> required index cached as success or Error
  -> interpreted content cached as success or Error
  -> visitor delivery (repeatable)
```

Payload-only callers do not force optional metadata decoding. A later visit reuses the required index. Repeated payload calls and visits reuse their cached results; they do not remap the path or clear a string arena.

Const operations mutate these caches. Sequential use is required, and external locking does not make views independent of the owning file lifetime.

## Commands and transitions

### Recognize

`isSupported(path)` case-normalizes only the extension. It performs no filesystem access and no content sniffing.

### Open

`open(path)` rejects an unsupported extension, maps a recognized path read-only, constructs the selected reader over the mapped span, and returns a move-only `File`. It does not parse metadata or prove that the content matches the extension.

### Extract encoded payload

`audioPayload()` computes or reuses the required format index and returns one non-empty `PayloadView`. It does not decode samples, normalize frames, or remove codec-internal padding.

### Visit interpreted fields

`visit(visitor)` first requires successful payload selection, then computes or reuses interpreted content. Only after both operations succeed does it deliver callbacks. Therefore a required failure cannot leave a partially mutated consumer.

Callbacks are synchronous. Their string and byte arguments may be retained only while the same backing `File` remains alive. Calling `visit()` again does not invalidate earlier views.

### Runtime adaptation

`readMediaTrack(path)` opens one `File`, creates an empty `TrackBuilder`, and maps callbacks directly into its metadata, property, and cover builders. The returned `MediaTrack` retains the `File` for the lifetime of its member builder. A copied or moved-out builder that still contains media-derived views must not outlive that `MediaTrack`. Runtime callers may mutate URI, tags, or custom metadata after reading, but those values have independent owners, are library concerns, and are never parsed as a generic public media aggregate.

## Required and optional format behavior

### FLAC

The first metadata block must be one exact 34-byte `StreamInfo`; all block boundaries and the final-block transition are required. Bytes must remain after the final metadata block. A bounded malformed Vorbis-comment or picture block is omitted atomically; other valid blocks and required technical properties remain available. Declared optional-entry counts do not cause allocation before the corresponding bounded entries are validated.

### MP4 audio

Every traversed atom must have a complete compact or 64-bit extended header. Recognized semantic boxes read their fields relative to the actual payload and therefore accept either header form. A top-level size `0` atom consumes the remainder of the file; size `0` inside a container, truncated headers, undersized declarations, and boundary overruns are rejected. Path lookup and audio-track selection validate only through the first complete match, while required payload indexing traverses every top-level atom and requires exactly one non-empty `mdat`. Metadata atoms, `mdta` key tables, freeform fields, cover children, and optional timing/property evidence are accepted only within their validated bounds. A malformed optional entry does not overwrite an earlier accepted value, and a malformed multi-image `covr` contributes no image.

### MPEG audio and ID3

A confirmed MPEG audio frame is required. A table-rate candidate is confirmed by a compatible adjacent frame when enough trailing bytes remain; an exact terminal frame is also accepted. A free-format candidate derives its frame length and bitrate from compatible adjacent frame boundaries. A valid leading ID3v2 envelope and valid trailing ID3v1/APEv2 regions are excluded from the payload. A malformed or oversized leading ID3 envelope may be ignored when a confirmed MPEG frame can still be located. A bounded but malformed ID3 frame sequence contributes no ID3 metadata; MPEG technical properties remain available. Unknown frames and unsupported `TXXX` keys are ignored.

### RIFF/WAVE

A valid supported WAVE format and non-empty `data` chunk are required. `LIST/INFO` fields are applied only after the entire bounded list validates. A malformed embedded ID3 chunk contributes no ID3 fields. WAVE technical properties and other valid optional evidence remain available.

## Failure and cancellation

- Unsupported extension: `Error::Code::NotSupported` from `open()`.
- Mapping failure: `Error::Code::IoError` with path context.
- Recognized container with a valid but unsupported codec, sample representation, channel count, or bit-depth combination: `Error::Code::NotSupported` from required parsing.
- Malformed required bytes or absent/empty payload: a recoverable `CorruptData` or `FormatRejected` result according to the rejected structure.
- Safely bounded malformed optional evidence: successful operation with that subtree omitted.
- Unknown or absent optional evidence: ordinary omission.

Format readers return `Result` directly and contain expected external-data failures without a public or private tag-exception hierarchy. Allocation failure and invariant faults are not converted to corruption.

File reading is synchronous and has no cancellation argument. Callers add cancellation around longer work such as payload hashing. Cancellation never changes a cached parser result and never publishes a partial identity.

## Persistence and versioning

This boundary writes no persistent state and never modifies source media.

Changing a normalized field mapping changes future import behavior and requires reference and fixture updates. Changing an encoded payload boundary changes persisted identity meaning and requires a database version change or an explicitly specified compatible re-index policy.

## Frontend observations

There is no direct frontend observation. Runtime library workflows translate read outcomes into their own mutation, scan, progress, and reporting contracts. Frontends neither invoke visitors nor hold mapped-file views.

## Implementation map

- [`File.h`](../../../include/ao/media/file/File.h), [`Visitor.h`](../../../include/ao/media/file/Visitor.h), and [`File.cpp`](../../../lib/media/file/File.cpp) own the public commands and shared state.
- Format readers under [`lib/media/file/`](../../../lib/media/file/) own format-specific required indexes, optional parsing, and payload selection.
- Container primitives under [`include/ao/media/`](../../../include/ao/media/) and [`lib/media/`](../../../lib/media/) own reusable byte layouts and lower parsing.
- [`readMediaTrack` and `MediaTrack`](../../../app/runtime/library/MediaTrack.h) own runtime adaptation to `TrackBuilder` and its backing lifetime.

## Test map

- [`FileTest.cpp`](../../../test/unit/media/file/FileTest.cpp) protects dispatch, mapping, failure atomicity, and move lifetime.
- Format tests under [`test/unit/media/file/`](../../../test/unit/media/file/) protect mappings, properties, covers, payloads, caches, and malformed input.
- [`FileTest.cpp`](../../../test/integration/media/file/FileTest.cpp) protects real encoded fixtures across all supported formats.
- [`MediaTrackTest.cpp`](../../../test/unit/runtime/library/MediaTrackTest.cpp) protects the runtime retention contract.

## Related documents

- [System architecture](../../architecture/system-overview.md)
- [Encoded media architecture](../../architecture/encoded-media.md)
- [Library architecture](../../architecture/library.md)
- [Supported audio files](../../reference/media/audio-file.md)
- [Library scan and audio identity](../library/runtime/scan-and-identity.md)
- [Decoder session](../playback/decoder-session.md)
- [Outcome channels](../failure/outcome-channel.md)
