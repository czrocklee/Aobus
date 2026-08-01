---
id: decision.0002.fail-closed-library-integrity
type: decision
status: accepted
domain: library
summary: Rejects safely detected persisted dictionary, Track, and manifest corruption at open or operation boundaries instead of exposing salvage rows or degraded runtime state.
---
# Decision 0002: fail closed at the library integrity boundary

## Context

The July 2026 error-handling remediation review began with separate proposals for corrupt manifest-row variants, partial Track projection, raw-key resume, reconstruction probes, and degraded presentation behavior.
Repeated review made each local protocol more complete but also made the product pay a general recovery-framework cost for physically damaged databases, even though Aobus is a single-user desktop music application and the existing architecture promises neither in-process repair nor degraded operation.

The chair therefore accepted a smaller product boundary: data that Aobus can validate safely before runtime exposure should either satisfy the current persisted invariants or reject the complete open or enclosing operation.
External media inspection errors remain ordinary per-item scan failures; malformed persisted truth does not impersonate one.

## Decision

`MusicLibrary::open()` validates the current metadata version, dictionary key and uniqueness invariants, paired Track keys and canonical records, every dictionary reference needed for reconstruction, and every manifest key and value before it exposes a library.
The first safely detected failure returns `CorruptData` and no runtime, partial All Tracks source, hidden row, or salvage cursor is produced.

The same exact manifest validator guards point reads, iteration, and writes.
Only `NotFound` means absence.
A malformed iterator row stops the enclosing Result-producing operation or CLI command instead of being skipped, and a Writer validates before mutation.
Scan apply treats a planned existing Track whose hot/cold evidence is absent or invalid as `CorruptData`; it never falls through to creating a replacement identity.

This decision does not claim that every physical LMDB failure is recoverable.
Mapped-file faults and corruption detected below a safe typed boundary retain the existing fatal/exceptional behavior.

## Alternatives considered

### Yield a corrupt manifest item and continue

Rejected because consumers then need raw-key identity, resume semantics, sorting and grouping policy, duplicate-report prevention, and per-command decisions about whether a partial result is usable.
It would also make scan appear to repair a database whose user-authored state it cannot reconstruct.

### Hide only damaged Tracks and keep the runtime alive

Rejected because it requires a degraded membership contract for All Tracks, every saved List, deltas, counts, selection, playback, and projections.
That is a new product mode rather than error containment.

### Open first and validate lazily

Rejected because runtime sources could publish partial membership before a later read discovers corruption.
Every consumer would then need its own rollback or invalidation behavior.

### Add an in-process repair or salvage tool

Rejected for this remediation.
Safe repair requires format-specific authority over which facts may be discarded and how database-only curation is preserved; that product is not currently specified.

## Consequences

- A live runtime begins from one validated persisted snapshot and can materialize All Tracks before exposure.
- Dictionary/Track/manifest corruption has one small typed policy instead of row variants, degraded projection semantics, resume protocols, and repair state.
- Startup performs a linear validation pass over these databases.
- A library containing safely detected malformed covered records does not open, even when most rows are usable.
- Rebuilding from media files loses database-only curation such as edited metadata, Lists, tags, saved order, and embedded cover choices.
- Preserving that curation requires a usable YAML export or another backup created before damage; a damaged database cannot be assumed exportable.
- This decision adds no promise for `SIGBUS`, arbitrary LMDB faults, or records outside the explicitly validated set.

## Current authorities

- [Library architecture](../architecture/library.md)
- [Library database](../reference/library/storage/database.md)
- [Library mutations](../spec/library/runtime/mutation.md)
- [Library scan and audio identity](../spec/library/runtime/scan-and-identity.md)
- [Library YAML transfer](../spec/library/runtime/yaml-transfer.md)
- [Track sources](../spec/library/source/track-source.md)
- [CLI execution](../spec/cli/execution.md)

## Supersession

Not superseded.
