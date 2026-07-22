---
id: rfc.0021.nonblocking-cover-art
type: rfc
status: draft
domain: resource
summary: Proposes non-blocking cover-art reads and transforms for interactive consumers.
depends-on: none
---
# RFC 0021: Non-blocking cover-art delivery

## Problem

GTK thumbnail loading already reads and decodes on the worker pool.
GTK full-size images, TUI cover rendering, and MPRIS art export still read resource bytes and perform decode or file work synchronously on their event-loop thread.

Large, malformed, or slow resources can therefore stall interaction.
Those paths also lack common encoded-byte and decoded-pixel limits.

The stored bytes are immutable for one `ResourceId`, and runtime already copies them into an owned vector.
The missing boundary can reuse the existing library task and cancellation infrastructure.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Move interactive resource reads off GTK and TUI event-loop threads.
- Move GTK/TUI decode and MPRIS file writes off those threads.
- Reject stale completions when the selected resource or owning consumer changes.
- Bound encoded bytes, decoded dimensions/pixels, and generated output bytes.
- Preserve frontend ownership of pixbufs, terminal cells/PNG, and file URLs.

## Non-goals

- Add a new resource state service or share decoded image types/caches across frontends.
- Add resource garbage collection, integrity scans, MIME metadata, or database migration.
- Change CLI's explicitly synchronous resource commands.
- Guarantee that every supported decoder can inspect dimensions without allocation.

## Proposed design

### Runtime byte read

Add one asynchronous operation to the existing runtime library task boundary:

```text
loadResource(ResourceId, stop_token) -> Task<Result<optional<vector<byte>>>>
```

It reads on the worker pool, enforces the encoded-byte limit, owns the returned bytes, and resumes on the callback executor.
It does not cache, decode, or publish state.

### Consumer lifetime

Each frontend owner retains the existing cancellation/lifetime mechanism and a local generation for its selected resource.
A completion updates visible or external state only when the owner is alive and both resource id and generation still match.

No service-wide request identity is necessary.
Equal concurrent reads may be coalesced later only if measurement shows value; the first implementation favors a direct operation.

### Frontend work

- GTK full-size delivery performs pixbuf decode/scale on a worker and returns the pixbuf through the GTK callback boundary.
- TUI performs block or Kitty conversion on a worker and renders the previous/empty image until completion.
- MPRIS reads and writes its derived cache file on a worker, then publishes a URL only for the still-current now-playing resource.

Thumbnail loading keeps its current coalescing implementation.
It may adopt the common byte-read operation only when doing so does not weaken its cancellation and cache behavior.

### Limits

The resource delivery specification owns conservative constants for encoded bytes, width/height, total decoded pixels, and generated PNG/file bytes.
Decoders that cannot establish safe bounds before a large allocation are rejected on the interactive path.

## Alternatives

### Patch each frontend with direct storage reads on ad hoc workers

This moves stalls but duplicates the transaction, byte ownership, error, and cancellation handoff.
One existing runtime task operation is the smallest shared boundary.

### Add a shared decoded-image service

GTK pixbufs, TUI pixels, Kitty PNG, and MPRIS files have different types and cache keys.
Sharing them would pull presentation dependencies into runtime.

### Add cache/coalescing immediately

Immutability permits it, but no measurement currently proves that duplicate raw reads are the dominant cost.

## Compatibility and migration

Resource ids and bytes do not change.
Images may appear after a short asynchronous delay, and over-budget input becomes an empty-image result instead of a long stall or excessive allocation.

## Validation

- Event-loop tests prove the three migrated paths perform no database read, decode, encode, or file write on the frontend thread.
- Cancellation and generation tests reject stale completion after resource change, widget/controller destruction, library replacement, and now-playing change.
- Boundary tests cover missing resources and encoded-byte, dimension, pixel, and output limits.
- Existing successful GTK, TUI, and MPRIS rendering/export fixtures remain valid.
- No runtime image type, request state, global cache, or resource-GC behavior is introduced.

## Promotion plan

Update the resource-delivery and runtime-execution architectures with the asynchronous byte-read boundary.
Update the cover-art delivery specification with limits, cancellation, and per-frontend completion behavior.
