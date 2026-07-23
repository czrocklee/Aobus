---
id: resource.blob
type: reference
status: current
domain: resource
summary: Enumerates ResourceId, immutable blob creation, collision probing, scoped and owned read surfaces, and raw administrative export.
---
# Resource blob

## Scope and version

This reference owns the exact logical surface of library resource identities and immutable raw blob operations.
The library database reference owns the physical LMDB database and raw record placement; the [cover-art delivery specification](../../spec/resource/cover-art-delivery.md) owns end-to-end behavior.

There is no independent resource schema version; compatibility follows the library database format version.

## Code boundary

`ResourceId` and `ResourceStore` belong to the **Core libraries** layer in the [system architecture](../../architecture/system-overview.md), under the [resource delivery](../../architecture/resource-delivery.md) and [library](../../architecture/library.md) architectures.
The runtime reader exposes an owned copy without leaking LMDB transaction spans.

## Identity surface

`ResourceId` is a strong type over unsigned 32-bit integer.
`0` is `kInvalidResourceId` and never names a resource row.
Ids are meaningful only inside the music library that owns the `resources` database.

The initial candidate is the low 32 bits of one-shot XXH3-64 over the complete bytes.
An initial zero candidate becomes `1`.
Collision probing increments the key and wraps from `UINT32_MAX` to `1`.

An id is content-derived but is not a portable cryptographic content identifier: a collision can assign later content a probed id, and unrelated libraries can assign the same id to different bytes.

## Store surface

| Role | Operation | Result |
|---|---|---|
| Reader | `begin()` / `end()` | Input iteration over `(ResourceId, borrowed bytes)` in database order. |
| Reader | `get(id)` | Optional transaction-borrowed byte span. |
| Reader | `maxKey()` | Greatest current numeric key or the underlying empty-store value. |
| Writer | `get(id)` | Optional write-transaction-borrowed byte span. |
| Writer | `create(bytes)` | Existing equal-content id or newly created id; typed error on storage failure/exhaustion. |
| Writer | `remove(id)` | `true` when a row existed and was removed. |
| Writer | `clear()` | Typed result from clearing all rows. |

Resource values are exact raw bytes with no header, length prefix, MIME, file extension, dimensions, refcount, or checksum field.
LMDB supplies the row length.

`LibraryReader::loadResource(id)` returns `optional<vector<byte>>` and copies before its scoped transaction ends.
CLI resource export writes that exact vector without interpretation.

`LibraryTaskService::loadResourceAsync(id, stopToken)` is the interactive owned-byte operation:

| Input/result | Exact behavior |
| --- | --- |
| `kInvalidResourceId` | successful `nullopt` |
| missing id | successful `nullopt` |
| encoded size `<= 33,554,432` bytes | successful owned byte vector |
| encoded size `> 33,554,432` bytes | `ValueTooLarge` |
| cancellation at an executor transition | throws `OperationCancelled` |
| successful or error result affinity | callback executor |
| library task progress/completion events | none |

The transaction-borrowed span is never returned or retained.
This operation does not decode, cache, or mutate resources.

## Validation rules

- Create never returns id `0`.
- Existing bytes are compared in full before an occupied candidate is reused.
- Unequal bytes at a candidate continue linear probing.
- Exhausting every nonzero key returns `ResourceExhausted`.
- Reader absence is `nullopt`; an invalid id is not automatically converted to a different resource.
- A borrowed Core span is valid only for the transaction that produced it.
- Track record validation separately rejects invalid resource ids in cover entries.

## Compatibility and versioning

Changing id width, invalid sentinel, initial hash, collision probe, or raw-value meaning changes the library storage contract and requires a database version decision.
Changing only a frontend decode or derived cache format does not change this surface.

No current reference-count or garbage-collection field protects deletion.
Mutation owners must not remove a row still referenced by a track.

## Examples

If hashing bytes produces candidate `0`, creation starts at id `1`.
If id `42` contains equal bytes, creation returns `42` without writing.
If id `42` contains different bytes and `43` is free, creation writes and returns `43`.

## Implementation authority

- [`CoreIds.h`](../../../include/ao/CoreIds.h) defines the strong type and invalid sentinel.
- [`ResourceStore.h`](../../../include/ao/library/ResourceStore.h) defines reader and writer operations.
- [`ResourceStore.cpp`](../../../lib/library/ResourceStore.cpp) defines create, deduplication, and probing.
- [`LibraryReader.cpp`](../../../app/runtime/library/LibraryReader.cpp) defines the owned runtime read.
- [`LibraryTaskService.cpp`](../../../app/runtime/library/LibraryTaskService.cpp) defines the bounded interactive owned-byte read.

## Test authority

- [`ResourceStoreTest.cpp`](../../../test/unit/library/ResourceStoreTest.cpp) protects id creation, deduplication, collision probing, reads, removal, clear, and errors.
- [`TrackBuilderCoverArtTest.cpp`](../../../test/unit/library/TrackBuilderCoverArtTest.cpp) protects valid references in track preparation.
- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) protects exact raw export and missing-id reporting.
- [`LibraryTaskServiceTest.cpp`](../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) protects interactive size, ownership, affinity, absence, event silence, and cancellation.

## Related documents

- [Resource delivery architecture](../../architecture/resource-delivery.md)
- [Cover-art resource delivery](../../spec/resource/cover-art-delivery.md)
- [Library architecture](../../architecture/library.md)
- [Library database](../library/storage/database.md)
- [Track model](../library/model/track.md)
