---
id: media.file-interpretation
type: spec
status: current
domain: media
summary: Defines supported-file recognition, mapping, metadata interpretation, encoded-payload extraction, containment, and borrowed-result lifetimes.
---
# Media file interpretation

## Scope

This specification defines current behavior for recognizing and opening supported encoded audio files, interpreting their metadata and technical properties, extracting covers and encoded-audio payload ranges, and containing malformed external input.
The [supported audio files reference](../../reference/media/audio-file.md) owns the exact extension, tag, codec, cover-role, and payload-range inventories.

Library scan reconciliation and identity publication belong to the [library scan and audio identity specification](../library/runtime/scan-and-identity.md).
PCM decoding belongs to the [decoder session specification](../playback/decoder-session.md).

## Code boundary

This contract belongs to the **Core libraries** layer in the [system architecture](../../architecture/system-overview.md), under the [media interpretation architecture](../../architecture/media-interpretation.md).
The public interpretation boundary is `include/ao/tag/TagFile.h`; format implementations are under `lib/tag/` and consume reusable parsers from `include/ao/media/` and `lib/media/`.
Runtime consumers may retain or materialize results, but do not redefine parsing behavior.

## Terminology

- **Supported path**: a path whose case-normalized extension occurs in the single `TagFile` dispatch table.
- **Interpreted track**: the current `library::TrackBuilder` candidate produced by one successful `loadTrack()` call.
- **Encoded payload**: the non-tag byte range selected by the container reader for audio identity; it is not decoded PCM.
- **Borrowed result**: a string or byte span whose storage belongs to the mapped file or the `TagFile` owned-string arena.

## Invariants

- `isSupported()` and `open()` derive recognition from the same dispatch table.
- A successful `open()` returns a non-null format reader whose file mapping is usable.
- An interpreted track is initialized as an empty builder and populated only with evidence accepted from the selected reader.
- The encoded payload is non-empty and lies within the mapped file.
- `audioPayload().offset` identifies the beginning of `bytes` within that mapped file.
- Returned borrowed values never outlive the `TagFile` storage that backs them.
- Required malformed structure is a recoverable failure; optional evidence may be skipped only where the format contract explicitly makes it best-effort.

## State model

`TagFile` owns one read-only file mapping, the cached mapping result, the selected format-reader object, and an arena of parser-created strings.
It is non-copyable and non-movable.

The mapping and payload spans remain valid for the object lifetime.
Each `loadTrack()` begins a new interpretation generation by clearing the owned-string arena, so strings from the previous generated arena become invalid at that point.
Mapped-file views remain tied to the object lifetime but callers still treat the complete builder as one generation-bound borrowed candidate.

## Commands and transitions

### Recognize and open

`isSupported(path)` case-normalizes the extension and returns whether a reader exists.
It does not access the filesystem or validate file content.

`open(path)` uses the same extension lookup, constructs the selected reader, maps the file read-only, and validates the mapping result before returning.
The exact recognized extensions and reader mapping belong to the reference.

### Interpret metadata

`loadTrack()` first verifies the mapping result and then delegates to the selected reader.
The reader clears its generated-string arena, creates an empty `TrackBuilder`, parses required structure, and applies recognized metadata, technical properties, tags, custom metadata, and cover entries.

Unknown optional metadata remains absent.
Malformed optional MP4 timing evidence used only for technical-property enrichment may be ignored; malformed required container or metadata structure fails the operation.
FLAC block iteration validates the current block before accepting the last-block marker.

The caller must serialize or otherwise materialize the builder before destroying the `TagFile` or invoking `loadTrack()` again on the same object.

### Extract encoded payload

`audioPayload()` first verifies the mapping and then delegates to the selected reader.
The reader excludes the tag and container regions defined by the reference and returns one borrowed non-empty byte span with its file offset.
It does not decode, normalize, or remove codec-internal padding from that span.

## Failure and cancellation

An unsupported extension returns `NotSupported` from `open()`.
A file mapping failure returns `IoError` with path context.
Malformed required external bytes and invalid or empty payload ranges return a recoverable format or `CorruptData` result according to the producing parser boundary.

Format implementations may use the private `tag::detail::TagException` and `media::detail::MediaException` for local control flow.
Public format boundaries catch only their declared private leaves and preserve the contained `Error`; unrelated exceptions are not converted to external-data failures.
FLAC interpretation translates both tag and shared-media parser leaves, while the current MP4, MPEG, and WAVE tag paths translate their tag leaf.
The MP4 demux result boundary translates its shared-media leaf.

Recognition, opening, interpretation, and payload extraction are synchronous and do not accept cancellation.
Callers add cancellation around longer workflows such as hashing or scan planning without publishing partial interpretation results.

## Persistence and versioning

This contract writes no persistent state directly.
Runtime library workflows may commit the interpreted candidate and derived identity after complete validation.

Changing a tag mapping changes future import behavior and requires corresponding reference and test updates.
Changing an encoded payload boundary changes persisted identity semantics and requires a library database version increment or an explicit compatible re-index policy.

## Frontend observations

There is no direct frontend observation from this Core boundary.
Runtime library tasks translate interpretation outcomes into scan plans, mutation results, progress, completion, or reports under their own specifications.
Frontends do not render parser-private exceptions or hold borrowed parser state.

## Implementation map

- [`TagFile.h`](../../../include/ao/tag/TagFile.h), [`TagFile.cpp`](../../../lib/tag/TagFile.cpp), and [`Open.cpp`](../../../lib/tag/Open.cpp) own the public behavior.
- Format readers under [`lib/tag/`](../../../lib/tag/) own format-specific interpretation and payload selection.
- [`MediaError.h`](../../../include/ao/media/detail/MediaError.h) and [`TagError.h`](../../../include/ao/tag/detail/TagError.h) own private containment leaves.
- Container primitives under [`include/ao/media/`](../../../include/ao/media/) and [`lib/media/`](../../../lib/media/) own shared byte interpretation.

## Test map

- [`TagFileTest.cpp`](../../../test/unit/tag/TagFileTest.cpp) protects extension dispatch and mapping failure.
- Format tests under [`test/unit/tag/`](../../../test/unit/tag/) protect metadata, technical properties, covers, payload ranges, and malformed input.
- Media tests under [`test/unit/media/`](../../../test/unit/media/) protect shared container validation.
- [`TagTest.cpp`](../../../test/integration/tag/TagTest.cpp) protects end-to-end interpretation and borrowed-builder use with encoded fixtures.

## Related documents

- [Media interpretation architecture](../../architecture/media-interpretation.md)
- [Supported audio files reference](../../reference/media/audio-file.md)
- [Library scan and audio identity](../library/runtime/scan-and-identity.md)
- [Decoder session](../playback/decoder-session.md)
- [Outcome channels](../failure/outcome-channel.md)
