---
id: rfc.0020.decoupled-media-interpretation
type: rfc
status: draft
domain: media
summary: Proposes a representation-neutral media interpretation boundary with owned results, explicit capabilities, and an acyclic Core dependency graph.
depends-on: none
---
# RFC 0020: Decoupled media interpretation

## Problem

The current media/tag boundary parses external audio files successfully, but its public abstraction is coupled to a library write representation.
`include/ao/tag/TagFile.h` includes `ao/library/TrackBuilder.h` and returns `library::TrackBuilder`, so `ao_tag` semantically depends on `ao_library` even though `lib/tag/CMakeLists.txt` declares only `ao_media` and `ao_utility`.

The library identity implementation creates the reverse edge: `lib/library/AudioIdentity.cpp` includes `TagFile.h`, opens a tag reader, and hashes its borrowed payload.
The real source-level graph is therefore `ao_tag -> ao_library -> ao_tag`, while the CMake target graph presents no such cycle.
Layer guardrails cannot enforce a direction the build graph does not express.

The result type also leaks a fragile lifetime protocol.
An interpreted `TrackBuilder` may borrow strings and cover spans from the mapped file and from a `TagFile` string arena that is cleared by the next `loadTrack()` call.
Every consumer must know that it must serialize or copy the candidate before file destruction or reuse.
Runtime scan and import code sometimes retains `TagFile` beside the builder to uphold this implicit contract.

Recognition, metadata interpretation, payload extraction, and decoder capability are related but not represented as explicit independent capabilities.
`TagFile`'s extension table is correctly authoritative for scanning, while decoder dispatch has its own supported surface.
There is no shared value that can answer which operations a format supports without tempting consumers to assume that tag import and PCM decoding are identical sets.

Media and tag failures are contained through narrow private exceptions, but translation ownership is distributed among format implementations.
The behavior is documented, yet the public boundary does not expose a representation-neutral interpretation outcome on which library, identity, and future tooling can converge.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0004](0004-scalable-library-tasks.md).

RFC 0003's unified mutation owner should consume the new library adapter rather than reintroduce parser dependencies into storage commands.
RFC 0004's phased preparation should materialize owned interpretation values before any bounded commit phase and use the same capability and cancellation evidence.

## Goals

- Make the declared Core target graph match the public source dependency graph.
- Remove every `ao_tag` dependency on library builders, records, identities, transactions, and runtime types.
- Return an owned, representation-neutral interpretation result with no hidden mapped-file lifetime.
- Keep container primitives reusable by both metadata interpretation and audio decoding without making either behavior own the other.
- Represent format capabilities explicitly so recognition, metadata import, payload identity, and decode support cannot drift or be conflated.
- Give library ingestion one adapter that converts an interpretation result into a `TrackBuilder` and resource mutations.
- Preserve current supported formats, metadata mappings, cover order and roles, codec evidence, payload boundaries, and recoverable failure behavior during migration.
- Retain chunked, cancellable identity computation without creating a reverse dependency on the tag abstraction.
- Add dependency guardrails that fail when the semantic cycle is reintroduced.

## Non-goals

- Change the current supported extension or codec inventory.
- Redesign the stored track model, resource store, scan reconciliation, or mutation publication.
- Merge metadata interpretation with PCM decoder-session lifecycle.
- Require one parser implementation for every tag and decoder operation.
- Introduce lazy borrowed values into runtime or frontend APIs.
- Change audio-identity semantics without a separately reviewed compatibility and re-index plan.

## Proposed design

### Representation-neutral interpretation module

Introduce a Core module whose public values describe external media evidence without including library or audio-execution headers.
The final target name may remain `ao_tag` or become `ao_media_interpretation`, but its dependency rule is fixed:

```text
ao_media_interpretation -> ao_media -> ao_utility
ao_library_adapter      -> ao_media_interpretation + ao_library
ao_audio                -> ao_media
```

`ao_library` storage remains independent of external-file parsers.
The adapter may live in application runtime when it coordinates scan or import, or in a narrow Core integration target if non-runtime clients require it; it cannot be part of either endpoint target in a way that recreates a cycle.

### Owned interpretation result

Replace the public `TrackBuilder` return with an owned value equivalent to:

```text
InterpretedAudioFile
  format and codec evidence
  technical properties
  normalized metadata fields
  tags and custom metadata
  owned ordered cover entries
  optional encoded-payload descriptor or identity input
  non-fatal omitted-evidence diagnostics
```

All strings and cover bytes are owned by the result or by an explicit immutable backing object retained inside it.
No consumer must retain a parser object or remember a serialize-before-reload rule.
Move-only ownership is acceptable; undocumented borrowed spans are not.

The library adapter performs the only mapping from this value to `TrackBuilder`, including cover-resource staging and stored codec/property values.
The adapter completes materialization before entering a library write transaction.

### Explicit capability registry

Define one registry of normalized file formats and independent capabilities:

```text
recognize for library scan
interpret metadata and properties
extract stable encoded payload
decode to PCM
```

Each consumer requests the capability it needs.
The scan supported set is derived from interpretation capability, while decoder dispatch is derived from decode capability; equality between those sets is neither required nor assumed.

The registry owns extension normalization and stable format identity, not implementation objects or library policy.
Adding a capability requires its implementation and contract tests in the same change.

### Payload and identity boundary

Separate payload description from library identity policy.
The interpretation module returns either an owned payload view backed by an explicit mapped-file owner or a streaming payload descriptor that can hash bounded ranges without exposing parser internals.

A representation-neutral hashing helper computes payload length and signature with chunked cancellation.
The library adapter converts that evidence into `AudioIdentity` and decides when to persist or backfill it.
The parser does not include library identity types, and core library storage does not open tag readers.

Before switching implementations, golden payload-range and identity tests prove byte-for-byte equivalence for every supported format.
Any deliberate change follows the database compatibility policy instead of silently re-indexing under the same meaning.

### Failure boundary

Public interpretation operations continue to return `Result` with mapping, unsupported capability, malformed required input, and optional-evidence omission distinguished.
Private parser exceptions remain local implementation leaves and are translated once by the module boundary.

The owned result can carry bounded non-fatal diagnostics for skipped optional evidence without converting a usable file into failure.
Library workflow policy decides whether those diagnostics remain task detail, become reports, or are ignored; the parser does not publish application notifications.

### Migration sequence

1. Add golden format, metadata, cover, payload, identity, error, and lifetime tests around the current boundary.
2. Introduce representation-neutral value and capability types without changing consumers.
3. Make each format implementation produce the owned interpretation result and keep a temporary adapter to the old `TrackBuilder` API.
4. Move library baseline construction into one integration adapter and migrate scan, YAML transfer, and edit consumers.
5. Move payload hashing behind the representation-neutral boundary and remove the library-to-tag dependency.
6. Split or rename targets so CMake declares every remaining edge and add forbidden-include checks in both directions.
7. Remove the borrowed public API and temporary adapter after all consumers and tests use the new contract.

Each step keeps the current specification and reference behavior unless a separately reviewed compatibility change says otherwise.

## Alternatives

### Declare `ao_tag` and `ao_library` as mutually dependent

Static-library link groups could hide the target cycle, but they would preserve confused ownership and make both modules harder to reuse and guard.
A representation-neutral value removes the cause rather than formalizing the cycle.

### Move `TrackBuilder` into `ao_tag`

`TrackBuilder` represents stored library records, dictionary encoding, and mutation preparation beyond external-file interpretation.
Moving it would pull library concerns downward and rename the coupling rather than remove it.

### Keep borrowed results and document them more prominently

The current lifetime can be used safely, but every new asynchronous or multi-file consumer must reproduce the same retention protocol.
Owned results trade bounded allocation/copying for a much smaller correctness surface at an I/O-bound ingestion boundary.

### Put all media, tag, library, and decoder code in one target

One target eliminates visible cycles only by eliminating enforceable boundaries.
The systems have different consumers, behavior, and lifetimes, so a monolith would increase accidental coupling.

### Use one extension list for interpretation and decoding

That would be simple while the sets happen to overlap, but it makes an unsupported capability appear available when a future format supports only metadata or only decoding.
Independent capability flags retain one format identity without conflating operations.

## Compatibility and migration

The migration preserves user-visible recognition, imported values, cover ordering, codec values, and payload bytes.
Golden fixtures compare the old and new result for every supported format before the old API is removed.

The owned result changes internal C++ APIs and target dependencies without a source-compatibility promise.
Heavy development permits that break, but all in-tree consumers migrate atomically at the final removal step.

Audio identity remains compatible only when payload bytes and hash inputs are identical.
If a discovered parser bug requires changing the range, the implementation first updates the reference, storage version/re-index policy, and migration tests.

## Validation

- CMake and include-boundary tests prove that media interpretation includes no library, runtime, UIModel, or frontend headers.
- A reciprocal guard proves that core library storage does not include interpretation headers.
- Format fixtures compare exact metadata, properties, tags, custom metadata, covers, codecs, payload offsets, payload bytes, and errors.
- Lifetime tests destroy and reuse parser objects before consuming the owned result.
- Identity tests prove byte-for-byte and signature compatibility and cover cancellation before, during, and after chunk processing.
- Scan, YAML transfer, edit, and identity-index tests prove the single adapter is used and no duplicate recognition table remains.
- Decoder tests prove its media-primitives dependency remains independent from metadata interpretation.
- A full `./ao check` passes after target and consumer migration.

## Open questions

- Should the interpretation module retain the `ao_tag` name or use `ao_media_interpretation` to reflect properties and payloads beyond tags?
- Should cover bytes be owned directly by each result or by an immutable shared backing blob retained by the result?
- Should payload hashing live in the interpretation module or a lower representation-neutral media utility target?
- Which optional-evidence diagnostics need a stable public value rather than diagnostic-sink-only reporting?

## Promotion plan

If accepted and implemented:

- update the [media interpretation architecture](../architecture/media-interpretation.md) with the acyclic target graph, owned result, capability registry, and final lifetime boundaries;
- update the [media file interpretation specification](../spec/media/file-interpretation.md) with owned-result and capability behavior;
- update the [supported audio files reference](../reference/media/audio-file.md) with any final public types while preserving exact format facts;
- update the [library architecture](../architecture/library.md), [library scan and audio identity specification](../spec/library/runtime/scan-and-identity.md), and [playback architecture](../architecture/playback.md) at their integration boundaries;
- add an accepted decision only if module naming or the owned-versus-shared backing choice needs durable rationale beyond the implemented contracts; and
- update contributor dependency guidance and executable include-boundary checks for the new target graph.
