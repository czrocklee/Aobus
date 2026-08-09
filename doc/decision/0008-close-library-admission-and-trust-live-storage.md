---
id: decision.0008.close-library-admission-and-trust-live-storage
type: decision
status: accepted
domain: library
summary: Closes current-schema admission before exposure and aborts on storage facts that fail after that trust boundary.
---
# Decision 0008: close library admission and trust live storage

## Context

The August 2026 error-handling convergence review retained Decision 0006's fail-closed open boundary but found that its proof was incomplete.
Opening a write transaction created missing named databases before deciding whether an environment was new, current-schema validation did not close the catalog or cross-Store references, and revision bytes were neither range-checked nor protected from wrap.
After open, the LMDB and Store adapters still used a general exception for faults that supported writers and the admission gate had made impossible.
That exception could reach an application catch boundary and make continuation appear supported even though Aobus has no degraded-library or in-process repair mode.

Aobus is a single-user desktop music application whose live runtime assumes one coherent library snapshot.
Persisted input can be rejected before that runtime exists, while a failure of an already-proved fact means the process can no longer trust its storage view.

## Decision

`MusicLibrary::open()` is the sole recoverable admission boundary for the host-local database.
It enumerates the main LMDB catalog before any `MDB_CREATE`, initializes only a truly empty environment, gates future versions from the stable metadata prefix, and then requires the exact current-version catalog, database flags, metadata record set, local records, and cross-Store references.
Safely observed incompatibility returns `NotSupported`; safely observed current-schema corruption returns `CorruptData`.

The complete current-schema closure includes matching Track sides, Track-to-manifest bijection, dictionary and Resource references, List parent existence and acyclicity, and nonzero unique saved-order ids.
Stored List filter text remains opaque storage data and is not parsed by admission.
Orphan dictionary and Resource rows are valid because they do not weaken a reference.

After successful admission, ordinary read snapshots trust those facts.
Absence and cursor end remain normal values, but a later native read fault is an unconditional infrastructure failure and a later malformed Store row is an invariant failure; both abort through the Core fatal facility.
The same rule applies when a logical writer or revision-bound runtime operation observes a
present record whose admitted parent, paired Track side, manifest binding, Dictionary entry,
or Resource reference has disappeared or changed incompatibly.
Reads performed inside an active write transaction use the private transaction-failure carrier so the root owner can abort all staged effects before returning its typed operation error.
Caller lifetime misuse remains a caller contract failure.

Library revisions are candidate transaction state.
A write transaction computes exactly one successor without persisting it, exposes that candidate to its operation, and writes it immediately before the native commit.
Abort or failed commit leaves the prior revision durable.
Persisted zero and `UINT64_MAX` are invalid; creating a successor after the maximum valid committed value is an invariant failure before mutation.

Physical metadata access is private.
`MusicLibrary` and its transaction capabilities expose logical header and revision values, while the narrow library-identity restore operation remains part of the logical write port.

## Alternatives considered

### Return `Result` from every Store read

Rejected because it would make every caller handle storage corruption after the library had already established the opposite invariant.
It would also introduce partial traversal and rollback questions without providing a usable degraded mode.

### Keep general exceptions until application leaves

Rejected because a catch-all leaf can log and continue, while the failed storage snapshot is no longer trustworthy.
The general exception channel obscures whether a root transaction was terminalized.

### Validate records lazily

Rejected because access order would decide whether the same persisted database is a recoverable open failure or a live fatal fault.
Runtime sources could publish partial state before the failure.

### Reserve the revision at transaction creation

Rejected as a durable write because the value is transaction-local either way and LMDB permits only one writer.
After acquiring that writer, the transaction reads the durable revision in its native snapshot and keeps the exact successor only as an in-memory candidate until commit; this avoids an early metadata write and makes abort semantics explicit even when multiple `MusicLibrary` instances were opened at different revisions.

## Consequences

- Current-version open performs a linear full-schema validation pass before constructing the runtime graph.
- Track-to-manifest closure uses count comparison plus one manifest point read per Track and requires only constant Track-sized auxiliary memory.
- List parent validation uses memory proportional to the List count.
- Future versions with the stable metadata prefix return `NotSupported` before current-version catalog and exact-header checks, leaving migration as a separate future design.
- External mutation or physical damage after open ends the process instead of producing partial output or a recoverable application error.
- Native write faults still unwind only to the transaction owner, which aborts before exposing a typed error.
- A successfully committed revision is never zero or `UINT64_MAX`, and a failed transaction never consumes a revision.
- The public library surface no longer exposes a physical Metadata Store.
- This decision does not promise containment for mapped-file signals such as `SIGBUS`, database migration, UTF-8 policy, or LMDB map-full resizing.

## Current authorities

- [Library architecture](../architecture/library.md)
- [LMDB operation specification](../spec/storage/lmdb-operation.md)
- [Library database reference](../reference/library/storage/database.md)
- [Outcome channel specification](../spec/failure/outcome-channel.md)

## Supersession

Supersedes [Decision 0006](0006-validate-open-fail-fast-live-iterator.md).
It retains Decision 0006's complete pre-exposure validation rationale and replaces only its post-open general-exception unwind choice with the unified fatal facility from [Decision 0007](0007-unify-fatal-diagnostics-and-abort.md).
