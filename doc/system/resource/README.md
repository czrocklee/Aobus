---
id: architecture.resource-delivery
---
# Resource delivery architecture

## Find the contract for your change

- [Reading, caching, transforms, and stale results](cover-art-delivery.md)
- [Persistent identity and descriptor operations](../../reference/resource/blob.md)
- [Imported cover formats](../../reference/media/audio-file.md#cover-import)

## Scope

Resource delivery is the end-to-end path by which imported cover bytes become a durable content identity and later reach interactive frontends, CLI export, or MPRIS.
This page explains ownership and dependency boundaries across Core, runtime, UIModel, and frontends.
Exact descriptor layout and operations belong to the resource reference; ceilings, cache behavior, frontend state transitions, and cancellation belong to the cover-delivery specification.

## End-to-end model

```text
media picture bytes or imported descriptor
  -> Core ResourceStore: digest + byte length -> local ResourceId
  -> ordered track cover references -> primary ResourceId
  -> runtime verified byte reader
       |-> derived digest-keyed disk cache
       `-> carrier audio files from the reverse index
  -> AppRuntime encoded-byte memory cache
       |-> GTK / WinUI / AppKit image delivery
       |-> TUI block or Kitty transform
       |-> MediaPlayer / SMTC artwork
       `-> MPRIS cache file

CLI export -> the same verified runtime reader without the interactive ceiling
```

A resource descriptor names content by SHA-256 digest and records its byte length; it stores no image bytes, MIME type, dimensions, or rendering metadata.
`ResourceId` is a compact nonzero handle local to one library.
Tracks retain ordered `(ResourceId, PictureType)` references, and primary selection chooses the first front cover or otherwise the first entry.
The library owns those identities and references; presentation consumers carry only the selected id.

## Ownership by layer

### Core

`ResourceStore` creates or reuses digest-derived ids and persists descriptors.
Equal content in one library reuses one identity; digest collisions in the compact handle are resolved by probing.
Rows are append-only in production because deletion could break a probe chain and no reference count proves a row unreferenced.
The library never stores the encoded cover payload.

### Runtime

`CoreRuntime` owns the verified whole-payload reader.
It copies the descriptor and an immutable carrier-index snapshot under a short read transaction, closes the transaction, and only then reads the derived cache or opens carrier files.
Every candidate is evidence rather than authority: bytes are accepted only when their SHA-256 digest matches the descriptor.
A stale or unsatisfied reference remains unchanged.

Interactive reads feed one `AppRuntime`-owned encoded-byte memory cache shared by that runtime's consumers.
The cache coalesces equal requests, retains immutable owned bytes within bounded budgets, and may complete a hit synchronously.
CLI export uses the same verified source walk without the interactive byte ceiling.
YAML export reads descriptors, not cover payloads.

### UIModel and frontends

UIModel owns semantic placeholder choices and stable style values, not image decoding or toolkit geometry.
Frontends own decoding, transforms, native image objects, placeholder rendering, stale-result fences, and adapter-specific caches.
GTK, WinUI, AppKit, TUI, system-media adapters, and MPRIS therefore share resource identity and encoded bytes without sharing decoded representations.
Derived disk entries, pixbufs, native images, terminal PNGs, and MPRIS files are replaceable artifacts, never library truth.

## Dependency direction

- Core resource storage depends on LMDB and digest utilities, never runtime, UIModel, or image toolkits.
- Core does not reopen media files to satisfy a resource read; runtime owns carrier discovery and re-extraction.
- Runtime exposes ids and owned encoded bytes, not toolkit image types, terminal escapes, MIME policy, or file URLs.
- Composition roots supply the derived-cache directory; runtime does not choose a platform path.
- Frontends own transforms and presentation, while the platform-neutral async layer owns equal-key request coalescing and callback-interest lifetime.
- Media owns cover extraction; library owns ordered references and mutation; presentation owns display policy.

## Identity and data integrity

The descriptor digest is the sole content identity.
A declared byte length is descriptive evidence and never decides whether content is read or accepted.
A writer that actually hashes bytes may correct a disagreeing stored length; an unverified imported declaration cannot overwrite an existing row.
Digests are portable between libraries, but local `ResourceId` values are not.

No read transaction spans cache or file I/O, and runtime returns owned bytes after the transaction closes.
Cache or carrier corruption cannot substitute different content because every cold source is verified.
A missing source yields absence, not mutation or fallback to another id.
Changing id width, digest algorithm, derivation, probing, descriptor layout, or track-reference representation requires a library compatibility decision; changing a derived frontend cache does not.

## Asynchrony, cancellation, and teardown

Each async consumer establishes its current identity or generation before requesting because an encoded-byte cache hit may invoke its callback synchronously.
Equal work can be shared while callback interests remain independently cancellable.
Cancelling one interest does not discard successful work needed by another or prevent useful cache population.
A completion token identifies the exact flight generation so late work cannot retire a replacement request for the same key.

Runtime closes its read transaction before worker I/O and returns on the selected callback executor.
Frontend owners cancel interests/tasks and fence current identity before releasing widgets or runtime borrowers.
`ResourceBytes` owns shared immutable storage and can outlive cache eviction or destruction; the reader and callback runtime remain retained for an in-flight cache miss according to the memory-cache lifetime contract.

## Presentation boundary

An invalid resource id may select a frontend placeholder without creating a resource read.
For GTK and WinUI cover slots, a valid id hides the shared UIModel no-cover placeholder while bytes are pending or delivery fails, preventing a stored cover from being represented as absent.
AppKit's native artwork view instead draws its independent system-note fallback whenever it has no decoded image, and the TUI uses a separate compact absence line when no transformed artwork is available; neither is one of the shared UIModel placeholder styles.
MPRIS publishes no art URL until a verified current-resource file exists.
All adapters reject stale completion and never rewrite the stored cover reference after absence or decode failure.

The detailed frontend state machines, ceilings, cache budgets, and transforms are defined in [cover-art resource delivery](cover-art-delivery.md), not repeated here.

## Source boundaries

- Core descriptors and ids: [`ResourceLayout.h`](../../../include/ao/library/ResourceLayout.h) and [`ResourceStore.h`](../../../include/ao/library/ResourceStore.h).
- Verified runtime reading and carrier indexing: [`app/runtime/resource/`](../../../app/runtime/resource).
- Shared encoded-byte cache: [`app/include/ao/rt/resource/`](../../../app/include/ao/rt/resource).
- Placeholder semantics: [`CoverArtPlaceholder.h`](../../../app/include/ao/uimodel/presentation/CoverArtPlaceholder.h).
- Frontend adapters live under their GTK, WinUI, AppKit, and TUI application directories.

Detailed implementation and test inventories remain with the reference and behavior specification.

## Related documents

- [Architecture landscape](../README.md)
- [System architecture](../overview.md)
- [Encoded media architecture](../media/README.md)
- [Library architecture](../library/structure.md)
- [Presentation architecture](../presentation/README.md)
- [Cover-art resource delivery](cover-art-delivery.md)
- [Resource descriptor reference](../../reference/resource/blob.md)
- [Track model](../../reference/library/model/track.md)
- [Decision 0010: never write to an audio file](../../decision/0010-never-write-to-audio-files.md)
