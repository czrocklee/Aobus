---
id: rfc.0021.bounded-resource-delivery
type: rfc
status: draft
domain: resource
summary: Proposes shared asynchronous resource reads, bounded transforms, typed request lifetimes, and non-blocking GTK, TUI, and MPRIS delivery.
depends-on: none
---
# RFC 0021: Bounded asynchronous resource delivery

## Problem

Resource identity and raw storage are coherent, and GTK thumbnail loading already has useful worker, coalescing, cache-key, and stale-callback behavior.
The end-to-end delivery system nevertheless has no shared operational boundary.

Three normal interactive paths perform potentially expensive work on a frontend event-loop thread:

- GTK full-size `ResourceImageController::loadFullSize()` opens a runtime reader, copies the complete blob, decodes it with Gdk, and populates the cache synchronously;
- TUI reads, decodes, crops, scales, and sometimes PNG-encodes selected artwork synchronously in its render/event loop; and
- `MprisArtUrlCache::urlForResource()` reads bytes and performs directory, stale-file, write, flush, close, and URI work synchronously on the GTK main context.

The same immutable resource can be copied and decoded independently by track rows, detail, now playing, TUI, and MPRIS.
Only GTK thumbnails coalesce equal requests.
There is no shared byte-read cache, request identity, byte budget, decoded-pixel budget, admission policy, or memory-pressure response.

Resource rows store arbitrary bytes without MIME or dimensions.
Frontend decoders apply local checks, but a small compressed blob can request a very large decoded allocation before any cross-frontend product budget intervenes.
The current system therefore has inconsistent protection against oversized input and decompression bombs.

Failure and cancellation are also inconsistent.
Thumbnail interests can cancel independently, full-size/TUI/MPRIS work cannot, and MPRIS degrades to an empty URL after log-only file failure.
There is no typed request outcome that distinguishes missing resource, unsupported image, invalid bytes, size rejection, cancellation, stale interest, cache hit, and I/O failure.

Finally, resource lifetime integrity is implicit.
Ids are immutable and tracks refer to them, but the store has no reference count or current garbage-collection contract.
An administrative remove can create dangling references, and derived caches assume id-to-bytes stability without an executable integrity audit.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0004](0004-scalable-library-tasks.md), [RFC 0012](0012-structured-async-fault-diagnostics.md).

RFC 0003 must align any resource-reference/garbage-collection mutations with the committed library revision.
RFC 0004 should reuse byte and work budgets when resource transforms are part of scan/import preparation.
RFC 0012 should receive unexpected background transform faults without converting them into duplicate user-facing reports.

## Goals

- Move resource read, full-size decode, TUI transform, and MPRIS file export off frontend event-loop threads.
- Provide one frontend-neutral asynchronous raw-byte request boundary without moving platform decoding into runtime.
- Coalesce equal immutable resource reads and bound retained raw bytes.
- Give every request a typed identity, per-interest cancellation, and stale-result rule.
- Apply explicit compressed-byte, decoded-dimension, decoded-pixel, and transformed-output budgets before large allocations.
- Preserve transform-specific GTK/TUI/MPRIS caches with keys that include every output-affecting input.
- Define typed absence, rejection, cancellation, operational failure, and success outcomes.
- Make resource reference integrity auditable and define safe orphan reclamation if product needs it.
- Preserve current cover selection, stored bytes, `ResourceId`, and frontend rendering behavior where inputs fit budgets.

## Non-goals

- Store decoded pixbufs, terminal cells, PNGs, or file URLs in the library database.
- Make runtime depend on GTK, Gdk, FTXUI, Kitty, D-Bus, or platform paths.
- Turn `ResourceId` into a cross-library portable cryptographic identity.
- Add network artwork fetching.
- Change picture-type or primary-cover selection semantics.
- Require all frontends to use the same image decoder.

## Proposed design

### Runtime raw-resource service

Add a runtime `ResourceReadService` owned by `CoreRuntime` beside the library facade.
It exposes immutable byte requests keyed by the active library identity and `ResourceId`:

```text
request(ResourceId, ResourceReadOptions, stop token)
  -> ResourceRequestId
  -> ResourceReadOutcome
```

The service opens the short library read on a worker, copies bytes under a configured maximum, and resumes the completion on the caller's callback executor.
It never decodes an image or chooses presentation policy.

Equal in-flight reads coalesce.
Each consumer owns one interest that can cancel independently; the underlying read continues while another interest remains.
A successful read may enter a bounded immutable byte cache even when the initiating interest becomes stale.

Cache keys include the active library's durable identity or runtime generation in addition to `ResourceId` so an active-library switch cannot reuse another library's bytes.
Runtime teardown closes admission, cancels interests, waits for worker completion, and releases cached buffers before library destruction.

### Typed outcomes

The result distinguishes at least:

```text
Loaded(bytes, byte count, cache disposition)
Missing
Rejected(limit, observed evidence)
Cancelled
StorageFailure(error)
```

Platform transforms add their own typed outcomes such as unsupported format, malformed image, invalid dimensions, output budget rejection, and stale interest.
Expected stale/cancelled outcomes remain local; operational and policy reporting follows the failure/reporting architecture.

### Decode budgets and transform contracts

Define product defaults and injectable test limits for:

- maximum stored bytes copied for interactive image delivery;
- maximum source width, height, and total decoded pixels;
- maximum target width, height, and output bytes;
- maximum concurrent read and decode work;
- maximum retained raw-byte and decoded-cache cost.

Frontends inspect dimensions through their decoder before committing the full decoded allocation where the library permits it.
When a decoder cannot provide a safe probe, the adapter uses a bounded decoding facility or rejects the format under the interactive path.

GTK thumbnail and full-size transforms share request/lifetime scaffolding but retain distinct decode/cache keys.
TUI block and Kitty transforms key by resource, target geometry, mode, and decoder policy version.
MPRIS exports original bytes on a worker through atomic replacement and keys the derived file by library identity plus resource id, not resource id alone.

### Frontend binding

Provide a small frontend-neutral request handle whose destruction cancels only that interest.
Each consumer also captures its semantic generation:

- GTK widget binding generation;
- TUI selected-resource and geometry generation;
- MPRIS now-playing generation;
- application/runtime generation across library replacement.

A completion must pass both the service request identity and the consumer generation before changing visible or external state.

The GTK thumbnail implementation migrates first because it already proves coalescing and interest cancellation.
Full-size controller then uses the same worker boundary.
TUI converts resource selection into an asynchronous state update and renders the previous/placeholder frame while work runs.
MPRIS publishes metadata without art first and emits a later metadata change when a current export becomes ready.

### Resource integrity and reclamation

Add an integrity audit that walks every track cover reference and verifies the resource exists.
Mutation tests enforce that adding byte-backed covers writes blobs and references in one transaction.

If orphan reclamation is implemented, it runs through the unified mutation pipeline:

1. capture a revision-consistent reference set;
2. identify unreferenced resource ids;
3. revalidate against the current revision inside a bounded write transaction;
4. delete only still-unreferenced rows;
5. commit and publish an administrative receipt.

Direct deletion of a referenced resource remains invalid.
The read service relies on immutability and may retain already read bytes until normal eviction even after safe orphan deletion.

## Alternatives

### Keep each frontend independent and patch only blocking calls

Moving three calls to ad hoc workers removes visible stalls but duplicates request, cancellation, limit, and library-switch safety logic.
The shared raw-byte boundary centralizes only platform-neutral work while preserving independent transforms.

### Decode images in application runtime

That would require choosing a decoder and image representation above GTK/TUI and would pull presentation dependencies into runtime.
The proposal shares bytes and operational policy, not platform artifacts.

### Cache only decoded images in each frontend

Decoded caches help repeat display but still duplicate database reads and provide no common byte limit or request lifetime.
They remain valuable downstream of the shared service.

### Store MIME and dimensions with every resource immediately

Stored evidence could accelerate validation but requires a database migration and trusted parsing rules.
The proposal can begin with bounded probes; persistent evidence can be evaluated separately after its authority and compatibility are clear.

### Use filesystem paths instead of database resources

External paths are mutable, unavailable for embedded artwork, and break portable library/YAML semantics.
Immutable stored bytes remain the authority.

## Compatibility and migration

`ResourceId`, raw stored bytes, track cover entries, picture types, primary selection, and current supported frontend formats remain unchanged.
The migration changes execution and failure timing: images may appear asynchronously where a synchronous path previously blocked, and over-budget images become an explicit no-image outcome.

MPRIS art URL filenames gain library identity or runtime-generation isolation.
Old derived cache files may be deleted lazily; they are not durable user data.

Budgets require product defaults and user-observable degradation tests before rollout.
Fixtures at and around every limit prevent accidental rejection drift.

## Validation

- Event-loop tests prove GTK full-size, TUI, and MPRIS paths perform no library read, decode, PNG encode, or file write on the frontend thread.
- Deterministic executor tests cover coalescing, per-interest cancellation, all-interest cancellation, cache salvage, teardown, and active-library generation changes.
- Budget tests cover stored byte, dimensions, pixels, target size, output bytes, concurrency, and cache eviction.
- Fault injection distinguishes missing, storage failure, malformed/unsupported image, limit rejection, cancellation, stale completion, and export failure.
- Existing GTK, TUI, MPRIS, CLI, and resource-store fixtures preserve successful output.
- Integrity tests find missing references and prove orphan reclamation revalidation if that phase is implemented.
- A full `./ao check` passes after migration.

## Open questions

- Should the bounded raw-byte cache live in runtime or in a Core service injected into runtime?
- Which image limits are product constants versus user-configurable preferences?
- Can Gdk and stb safely probe dimensions before full allocation for every currently accepted format?
- Should MPRIS publish delayed art with a second `PropertiesChanged` or await a short bounded export window before initial metadata?
- Is orphan reclamation needed now, or is an integrity audit plus removal prohibition sufficient?

## Promotion plan

If accepted and implemented:

- update the [resource delivery architecture](../architecture/resource-delivery.md) with the asynchronous service, budget owners, request/generation graph, and final cache lifetimes;
- update the [cover-art resource delivery specification](../spec/resource/cover-art-delivery.md) with typed outcomes and asynchronous frontend transitions;
- update the [resource blob reference](../reference/resource/blob.md) only if public read/integrity surfaces change;
- update the [runtime execution](../architecture/runtime-execution.md), [library](../architecture/library.md), [presentation](../architecture/presentation.md), and [interactive session lifecycle](../architecture/interactive-session-lifecycle.md) architectures at their worker, consumer, and teardown boundaries;
- update MPRIS and TUI specifications with delayed-art observations and cancellation; and
- add development guidance and deterministic test helpers for resource budgets and frontend-thread assertions.
