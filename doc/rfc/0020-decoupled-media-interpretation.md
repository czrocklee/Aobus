---
id: rfc.0020.decoupled-media-interpretation
type: rfc
status: rejected
domain: media
summary: Rejected the owned interpretation aggregate and capability registry after a narrower zero-copy visitor boundary removed the media/library cycle.
depends-on: none
---
# RFC 0020: Decoupled media interpretation

## Disposition

Rejected on 2026-07-13.

The problem was real, but the proposed public owned result, format-capability registry, and stable omission-diagnostic model were broader than necessary. An independent refactor removed the cycle with a smaller implemented boundary:

- the `media::file` sub-boundary lives inside `ao_media` and never depends on library storage;
- `ao::media::file::File` owns the read-only mapping and exposes `visit(Visitor&)` plus `audioPayload()`;
- visitor arguments and payload bytes are zero-copy views whose lifetime is explicitly tied to the move-only `File`;
- `ao::rt::readMediaTrack` is the single private visitor-to-`TrackBuilder` adapter, and its `MediaTrack` result retains `File` for the builder lifetime; and
- `library::readAudioIdentity` accepts a byte span, so `ao_library` no longer opens media files.

The current structural boundaries are documented by the [encoded media](../architecture/encoded-media.md), [system](../architecture/system-overview.md), and [library](../architecture/library.md) architectures; reader behavior and exact surfaces are documented by the [specification](../spec/media/file-reading.md) and [reference](../reference/media/audio-file.md). Those current authorities supersede this proposal; this RFC remains only as the record of the rejected larger design.

## Problem

At proposal time, `ao_tag::TagFile` returned `library::TrackBuilder`, while `ao_library::AudioIdentity` opened `TagFile`. The semantic source graph was a hidden `ao_tag -> ao_library -> ao_tag` cycle even though the declared targets did not expose it.

The builder also borrowed mapped strings, cover bytes, and reader-owned decoded strings. Runtime consumers had to retain `TagFile` beside the builder and serialize before another load. Recognition, metadata reading, payload extraction, and decoding had no explicit capability model, and format readers repeated exception translation.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0004](0004-scalable-library-tasks.md).

The rejected proposal would have aligned its library adapter with RFC 0003 and its owned preparation phase with RFC 0004. The narrower implemented refactor does not require either RFC.

## Goals

The proposal sought to:

- make the declared Core dependency graph match source dependencies;
- remove parser dependencies on library records and the reverse identity dependency;
- eliminate hidden parser-lifetime obligations from returned values;
- introduce independent recognition, interpretation, payload, and decode capabilities;
- give library ingestion one adapter; and
- preserve format mappings, payload identity, and recoverable failure behavior.

The dependency, adapter, compatibility, and explicit-lifetime outcomes were achieved by the narrower refactor. The owned-result and registry goals were deliberately not adopted.

## Non-goals

- Change the supported extension, codec, field, cover, or payload inventories.
- Redesign stored tracks, resources, scan reconciliation, or mutation publication.
- Merge media-file reading with decoder-session lifecycle.
- Change persisted audio-identity meaning.
- Write metadata back to source media.

## Proposed design

The rejected design introduced a representation-neutral `InterpretedAudioFile` aggregate containing owned normalized fields, technical properties, covers, payload evidence, and bounded omission diagnostics. A shared registry would have described recognition, interpretation, payload, and PCM-decode capabilities. A library adapter would have converted the owned result to `TrackBuilder`, and a neutral hashing helper would have converted its payload descriptor to library identity.

This would have removed all borrower retention rules, but it also introduced a second media-domain representation, copied or reference-counted large media evidence, stabilized diagnostics with no current consumer, and added registry policy for four formats whose required reader surfaces are already explicit.

## Alternatives

### Zero-copy visitor with an owning file

This is the implemented alternative. It keeps one mapped owner, delays optional parsing until requested, emits normalized evidence directly, and makes the runtime adapter's retention relationship structural. It preserves zero-copy ingestion and removes the cycle without a public aggregate.

Tradeoff: consumers that retain visitor views must retain `File`. Production has exactly one builder adapter, and payload-only consumers already retain the file while hashing, so the obligation is narrow and mechanically represented.

### Keep `TrackBuilder` in the parser API

Rejected because it preserves the hidden Core cycle and makes storage representation part of external-file parsing.

### Introduce only an owned aggregate

Rejected for now because deep ownership copies artwork and decoded values, while shared backing merely hides the same mapped owner behind another domain type. A future asynchronous consumer may justify a focused owned snapshot, but no current consumer requires it.

### Introduce only a capability registry

Rejected for now because scan recognition already derives from the one `File` dispatch, while decoder dispatch intentionally remains independent. A registry becomes justified only when one normalized format has independently varying implemented capabilities that real consumers must query.

## Compatibility and migration

The implemented alternative preserved recognized extensions, normalized fields, cover order and roles, codec evidence, payload offsets and bytes, and identity hashing. All in-tree callers migrated atomically; there is no source-compatibility layer for `ao::tag`, `TagFile`, `TagError`, or `ao_tag`.

Mapped views now survive repeated visits and moving `File`; they remain invalid after its destruction. `File` is explicitly sequential and non-concurrent.

## Validation

The independent refactor was accepted only with:

- format regression tests for FLAC, MP4, MPEG/ID3, and WAVE;
- exact payload and identity tests over the shared fixture set;
- malformed optional-subtree atomicity tests;
- a public guarantee that required failure emits zero visitor callbacks;
- move-lifetime coverage for mapped views and the runtime adapter;
- scan, YAML, direct-create, and identity-index consumer migration; and
- full repository validation.

## Open questions

None. The larger proposal is rejected. Future owned snapshots, capability discovery, or stable optional-evidence diagnostics require a new evidence-backed RFC rather than reopening this document implicitly.

## Promotion plan

No proposal promotion remains. The narrower implemented design is current in:

- [System architecture](../architecture/system-overview.md)
- [Encoded media architecture](../architecture/encoded-media.md)
- [Library architecture](../architecture/library.md)
- [Media file reading specification](../spec/media/file-reading.md)
- [Supported audio files reference](../reference/media/audio-file.md)
- [Library scan and audio identity specification](../spec/library/runtime/scan-and-identity.md)
