---
id: library.reference-index
type: index
status: current
domain: library
summary: Routes exact library entity, field, storage, and interchange surfaces.
---
# Library reference

## Purpose

This area is the lookup surface for exact library fields, entity shapes, identifiers, storage records, and serialized formats.

## Responsibilities

- Track metadata, technical properties, cover entries, codec values, and field capabilities.
- List kinds, fields, membership representation, and identifier rules.
- Links to the physical LMDB layout and portable YAML format.

## System context

This reference is the exact-surface companion to the Library stages in [system architecture](../../architecture/system-overview.md):

| Surface | Code boundary | Reference area |
|---|---|---|
| Core domain model | `include/ao/library/` | [`model/`](model/track.md) |
| Runtime field catalog | `app/include/ao/rt/TrackField.h`, `app/runtime/TrackField.cpp` | [`model/`](model/track-field.md) |
| Core physical storage | `include/ao/library/*Layout.h`, `lib/library/` | [`storage/`](storage/database.md) |
| Runtime interchange boundary | `app/runtime/library/` | [`format/`](format/yaml.md) |
| External media-file reading | `include/ao/media/file/`, `lib/media/file/`, and reusable `lib/media/` primitives | [Supported audio files](../media/audio-file.md) |

## Out of scope

Behavioral transitions belong to [library specifications](../../spec/library/README.md), and ownership belongs to [library architecture](../../architecture/library.md).

## Document map

### Model

- [Track model](model/track.md) enumerates persisted metadata, technical values, cover entries, codec values, tags, and custom metadata.
- [Runtime track field catalog](model/track-field.md) enumerates application field ids, labels, capabilities, sort/group mappings, completion flags, and query bridges.
- [List model](model/list.md) enumerates list fields, kinds, identifiers, and stored membership shape.

### Storage

- [Library database](storage/database.md) defines the host-local LMDB environment, keys, records, and version gate.

### Format

- [Library YAML format](format/yaml.md) defines the portable, fail-closed version 2 interchange shape.
- [Supported audio files](../media/audio-file.md) defines recognized extensions, imported tag mappings, and encoded payload ranges under the media owner.

## Recommended reading paths

- Storage work: database, then track and list models.
- Metadata work: track model, runtime field catalog, then supported audio files.
- Transfer work: YAML format, then track and list models.

## Implementation and test map

- Core entity and layout definitions: `include/ao/library/`.
- Runtime field catalog: `app/include/ao/rt/TrackField.h` and `app/runtime/TrackField.cpp`.
- Core library tests: `test/unit/library/`.
