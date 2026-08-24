---
id: resource.blob
type: reference
status: current
domain: resource
summary: Enumerates ResourceId derivation, the descriptor row, digest-keyed creation, collision probing, and the runtime materialization surface.
---
# Resource descriptors

## Scope and version

This reference owns the exact logical surface of library resource identities and descriptor operations.
The library database reference owns the physical LMDB database and raw record placement; the [cover-art delivery specification](../../spec/resource/cover-art-delivery.md) owns end-to-end behavior.

The library stores no cover bytes. A resource row describes content and never holds it.

There is no independent resource schema version; compatibility follows the library database format version, which is 7.

## Code boundary

`ResourceId`, `ResourceDescriptor`, `ObservedResourceDescriptor`, and `ResourceStore` belong to the **Core libraries** layer in the [system architecture](../../architecture/system-overview.md), under the [resource delivery](../../architecture/resource-delivery.md) and [library](../../architecture/library.md) architectures.
The runtime reader exposes owned bytes materialized outside any transaction.

## Identity surface

Content is identified by its SHA-256 digest, computed over the encoded image payload a media reader's picture callback provides.
The digest is the whole identity: bytes are accepted as a resource if and only if they hash to it.

`ResourceId` is a strong type over unsigned 32-bit integer and is the compact local handle for that identity, because a track's 8-byte cover entry cannot carry 32 bytes.
`0` is `kInvalidResourceId` and never names a resource row.
Ids are meaningful only inside the music library that owns the `resources` database.

The initial candidate is the first four digest bytes, in the order SHA-256 produces them, read as a big-endian unsigned 32-bit integer.
An initial zero candidate becomes `1`.
Collision probing increments the key and wraps from `UINT32_MAX` to `1`, and stops at the first empty slot.

Handles collide where digests do not, which is what the probe resolves.
A digest is portable; a handle is not, so a transfer document carries the digest and every importing library derives its own handle from it.

## Descriptor surface

| Field | Type | Meaning |
|---|---|---|
| `digest` | 32 bytes | SHA-256 of the content; the sole identity. |
| `byteLength` | unsigned 32-bit | Length of the content the digest names. |

The persisted row is exactly 36 bytes: the digest, then the length as four bytes in the machine's own order, which is the record's object representation written whole.
The row has no header, MIME type, file extension, dimensions, refcount, or payload.

`byteLength` is a stored fact, never an admission rule: it lets a caller report a cover's size without holding it, and it never decides whether content is read.
Its evidence decides how it is written:

| Evidence | Writer | Effect on an existing row |
|---|---|---|
| Counted | The writer hashed the bytes it counted. | Corrects a disagreeing stored length. |
| Declared | A document stated it and nothing checked it. | Leaves the stored length alone; creates the row when absent. |

`ObservedResourceDescriptor` is the typed counted-evidence wrapper around a `ResourceDescriptor`.
It carries no payload and has no distinct persisted representation; it lets preparation hash and count bytes before writer ownership while preserving the counted update rule.

## Store surface

| Role | Operation | Result |
|---|---|---|
| Reader | `begin()` / `end()` | Input iteration over `(ResourceId, ResourceDescriptor)` in database order. |
| Reader | `get(id)` | Optional descriptor. |
| Reader | `maxKey()` | Greatest current numeric key or the underlying empty-store value. |
| Writer | `get(id)` | Optional descriptor under the write transaction. |
| Writer | `create(bytes)` | Existing id for equal content or a newly created id, with a counted length; `ValueTooLarge` above `UINT32_MAX`; typed error on storage failure or exhaustion. |
| Writer | `getOrCreate(descriptor)` | Existing id for that digest, or a newly created row with the declared length. |
| Writer | `getOrCreate(observed)` | Existing id for that digest, correcting a disagreeing length, or a newly created row with the counted length. |
| Writer | `remove(id)` | `true` when a row existed and was removed. No production path calls it. |
| Writer | `clear()` | Typed result from clearing all rows. |

Descriptors are append-only in practice: a rescan that replaces a track's covers leaves earlier rows in place, and a row displaced by a collision must stay reachable along its probe chain.
Removing one row from the middle of a chain turns a later row's path into a hole, which the open gate rejects as corruption.

`LibraryTaskService::loadResourceAsync(id, sizeLimit, stopToken)` is the runtime owned-byte operation, used by interactive delivery and by administrative export:

| Input/result | Exact behavior |
| --- | --- |
| `kInvalidResourceId` | successful `nullopt` |
| missing descriptor | successful `nullopt` |
| no cache entry and no carrier yielding the digest | successful `nullopt` |
| materialized size within the caller's ceiling | successful owned byte vector |
| materialized size above the caller's ceiling | `ValueTooLarge` |
| `ResourceSizeLimit::Interactive` | ceiling of `33,554,432` bytes |
| `ResourceSizeLimit::Administrative` | no ceiling |
| cancellation at an executor transition or between candidates | throws `OperationCancelled` |
| successful or error result affinity | callback executor |
| library task progress/completion events | none |

The read transaction that resolves the descriptor and the carrier snapshot is closed before any cache lookup or file open.
This operation does not decode or mutate resources, and it never rewrites a reference it could not satisfy.

## Validation rules

- Create never returns id `0`.
- An occupied candidate is reused only when its stored digest is equal.
- A different digest at a candidate continues linear probing.
- Exhausting every nonzero key returns `ResourceExhausted`.
- Content longer than `UINT32_MAX` returns `ValueTooLarge` and stores nothing.
- Reader absence is `nullopt`; an invalid id is not automatically converted to a different resource.
- Track record validation separately rejects invalid resource ids in cover entries.
- The library open gate rejects a resource value that is not exactly 36 bytes, two rows carrying one digest, and a row unreachable from its digest's initial key.

## Compatibility and versioning

Changing id width, invalid sentinel, digest algorithm, id derivation, collision probe, or descriptor layout changes the library storage contract and requires a database version decision.
Changing only a frontend decode or derived cache format does not change this surface.

No current reference-count or garbage-collection field protects deletion.
Mutation owners must not remove a row still referenced by a track, and no production path removes a row at all.

## Examples

If the first four digest bytes are zero, creation starts at id `1`.
If id `42` holds an equal digest, creation returns `42` without writing.
If id `42` holds a different digest and `43` is free, creation writes and returns `43`.
If a `full` document declares length `1024` for a digest whose row already holds `2048`, the row keeps `2048` until a writer hashes the content.

## Implementation authority

- [`CoreIds.h`](../../../include/ao/CoreIds.h) defines the strong type and invalid sentinel.
- [`Sha256.h`](../../../include/ao/utility/Sha256.h) defines the digest, its hexadecimal spelling, and parsing.
- [`ResourceLayout.h`](../../../include/ao/library/ResourceLayout.h) defines the descriptor, counted-evidence wrapper, persisted bytes, and id derivation.
- [`ResourceStore.h`](../../../include/ao/library/ResourceStore.h) defines reader and writer operations.
- [`ResourceStore.cpp`](../../../lib/library/ResourceStore.cpp) defines create, digest reuse, length evidence, and probing.
- [`LibraryYamlExporter.cpp`](../../../app/runtime/library/LibraryYamlExporter.cpp) defines the administrative scoped read used by export.
- [`LibCommand.cpp`](../../../app/cli/LibCommand.cpp) defines CLI resource listing and export.
- [`LibraryTaskService.cpp`](../../../app/runtime/library/LibraryTaskService.cpp) defines the runtime owned-byte read and the carrier snapshot it walks.

## Test authority

- [`Sha256Test.cpp`](../../../test/unit/utility/Sha256Test.cpp) protects published digest vectors, hexadecimal spelling, and parsing.
- [`ResourceLayoutTest.cpp`](../../../test/unit/library/ResourceLayoutTest.cpp) protects descriptor size, alignment, byte order, and id derivation.
- [`ResourceStoreTest.cpp`](../../../test/unit/library/ResourceStoreTest.cpp) protects id creation, digest reuse, collision probing, declared and counted descriptor evidence, reads, removal, clear, and errors.
- [`TrackBuilderCoverArtTest.cpp`](../../../test/unit/library/TrackBuilderCoverArtTest.cpp) protects valid references in track preparation.
- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) protects descriptor listing, export by materialization, and both absence reports.
- [`LibraryTaskServiceTest.cpp`](../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) protects interactive size, ownership, affinity, absence, event silence, cancellation, and carrier-index rebuild behavior.

## Related documents

- [Resource delivery architecture](../../architecture/resource-delivery.md)
- [Cover-art resource delivery](../../spec/resource/cover-art-delivery.md)
- [Library architecture](../../architecture/library.md)
- [Library database](../library/storage/database.md)
- [Track model](../library/model/track.md)
