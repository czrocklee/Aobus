---
id: decision.0014.admit-scan-plans-by-item-evidence
type: decision
status: accepted
domain: library
summary: Admits prepared scan items by commit-time evidence while preserving one atomic scan transaction.
---
# Decision 0014: admit scan plans by item evidence

## Context

The August 2026 scan hardening review found that live scan apply entered
`Maintenance` before media parsing and retained it through commit. This kept a
plan's exact library revision current, but it also disabled all interactive
authoring during the slowest filesystem, parsing, cover-hashing, and audio-
fingerprinting work.

Moving that preparation outside `Maintenance` exposed a second problem. An
exact revision gate would reject the complete prepared plan after any unrelated
edit, even when the edit did not affect a scanned URI. The same gate also made
later missing Track evidence an unconditional invariant failure. Without the
gate, an ordinary deletion during preparation could therefore reach a fatal
path unless scan-specific staleness was checked before mutation.

LMDB supplies one process-wide writer and no item savepoints that fit the
existing library abstraction. A scan also carries relationships whose partial
application is undesirable, especially a move that consumes an old binding and
creates a new one. The solution must improve interactivity without introducing
nested transactions or making a single scan visible as a sequence of partial
commits.

## Decision

A scan plan remains bound to one persisted library id and records the planner's
committed revision as provenance, but that revision is not commit authority.
The planner captures an owned, short-lived manifest snapshot and releases its
read transaction before filesystem traversal, file I/O, or hashing.

Plan application uses two evidence boundaries:

1. Outside the write transaction, it revalidates current filesystem facts.
   New and changed files must retain their planned size and modification time,
   missing paths must remain absent, and moved destinations must retain both
   their planned size and modification time and their exact audio identity.
2. Inside the single write transaction and before any mutation, it admits every
   actionable item against live database evidence. A new destination must
   remain absent. Existing items must retain the planned manifest key, Track
   binding, status, size, and modification time. Only moved source rows
   additionally retain their planned audio identity. A moved destination must
   remain absent.

Changed-file admission deliberately ignores curated Track metadata and a
manifest identity backfilled independently. Apply re-reads the live Track and
merges the prepared technical and cover facts into that current metadata.

Stale `New`, `Changed`, and `Missing` items are counted separately from failures
and skipped for a later scan. Stale `Moved` evidence reports a failure and
aborts the complete scan application because relinking is one identity-
preserving operation and explicit relink uses the same path. All database
checks happen in a pre-mutation pass, so supported user mutations cannot reach
the Track/manifest binding invariants later in the same LMDB write transaction.

Every admitted item is still applied in one root write transaction and one
publication. A recoverable post-effect error aborts that complete transaction.
There are no nested LMDB transactions, item savepoints, or partial batch
commits.

Long-running scan preparation and identity backfill hold a background-task
lease instead of closing authoring admission. The lease serializes them with
each other and with YAML import preparation, while each short write phase
reserves the coordinator writer before waiting for it. Callback-executor
authoring returns `Unavailable` while that reservation is pending or active;
other writers retain ordinary waiting semantics. YAML import retains its
stronger `Maintenance` behavior. The lease itself emits no authoring-
availability transition.

## Alternatives considered

### Keep `Maintenance` around complete preparation

Rejected because filesystem and hashing latency would continue to disable
unrelated metadata and List authoring for the duration of a large scan or
backfill.

### Release `Maintenance` but keep exact revision admission

Rejected because any unrelated edit would discard potentially minutes of valid
preparation. Bounded automatic retries would repeat that cost and could still
fail under sustained authoring.

### Commit each item or bounded item batch independently

Rejected because it would expose partial scan state, multiply revisions and
publication traffic, and require a new recovery model for failures between a
move's related effects. LMDB child transactions would not remove those product-
level semantics and are not exposed by the library transaction boundary.

### Treat all stale items alike

Rejected because a stale ordinary item can be skipped independently, while a
move is the plan's assertion that one old and one new path are the same audio
identity. Preserving the existing whole-scan abort for a stale move avoids a
partially accepted relink decision.

## Consequences

- Unrelated interactive edits may complete while scan media preparation is in
  progress and do not invalidate otherwise current scan items.
- A concurrent edit to the same URI becomes an ordinary stale-item skip rather
  than a reported failure, whole-plan revision conflict, or process abort.
- Curated metadata changed during preparation is preserved by the changed-file
  merge.
- Planning owns URI and manifest-record copies proportional to manifest size so
  it can release the LMDB read snapshot early.
- The final write transaction still scales with all admitted items and retains
  whole-plan rollback cost.
- Scan, backfill, and import preparation remain mutually exclusive through a
  non-presentational lease; only import changes authoring availability.
- Callback-owner authoring remains available during long preparation and fails
  fast when it observes that a background write is queued or active, preventing
  the ordinary contention path from waiting on the complete scan transaction.
- A scheduler-scale check-to-lock window remains if the callback owner passes
  its admission check immediately before a background writer reserves and
  acquires the physical writer. Eliminating that exceptional window would
  require coordinating all writer acquisition through a broader admission
  protocol; that complexity is not proportionate to this application's risk.
- The plan revision remains useful diagnostic provenance but no longer proves
  applicability.

## Current authorities

- [Library architecture](../architecture/library.md)
- [Library scan and audio identity specification](../spec/library/runtime/scan-and-identity.md)
- [Library task execution specification](../spec/library/runtime/task-execution.md)
- [Library change publication specification](../spec/library/runtime/change-publication.md)

## Supersession

This decision does not supersede
[Decision 0008](0008-close-library-admission-and-trust-live-storage.md).
Decision 0008 still owns post-open storage integrity. This decision distinguishes
ordinary scan staleness, proved by item evidence before mutation, from an
integrity breach after that evidence has been admitted inside one write
transaction.
