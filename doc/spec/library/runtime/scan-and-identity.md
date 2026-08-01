---
id: library.scan-identity
type: spec
status: current
domain: library
summary: Defines audio-file scan classification, reconciliation, move relinking, cancellation, and identity backfill.
---
# Library scan and audio identity

## Scope

This specification defines `LibraryScan`, scan plans, plan application, encoded-audio identity, automatic move relinking, and pending-identity backfill.
It owns filesystem-to-library reconciliation behavior rather than the physical manifest record.

Supported containers, imported tags, and encoded payload ranges belong to the [supported audio files reference](../../../reference/media/audio-file.md).
Manifest keys and fields belong to the [library database reference](../../../reference/library/storage/database.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
The runtime surface is `app/include/ao/rt/library/LibraryScan.h`, `ScanPlan.h`, and `LibraryTaskService.h`; planning, reconciliation, and backfill live in `app/runtime/library/`, while encoded-payload extraction and manifest storage remain core-library facilities.
`app/include/ao/uimodel/library/task/LibraryScanWorkflow.h` owns the frontend-shared build/classify/apply workflow, including the eager versus fast-bootstrap identity strategy.
`LibraryScan` is read-only plan construction.
Live committing application is a runtime-private coordinator operation.
The runtime-private `ScanApplyOperation::run()` is the distinct offline composition used by focused storage workflows and tests.

## Terminology

- **Manifest URI** is the normalized music-root-relative key bound to one track.
- **Audio identity** is encoded-audio payload length plus its XXH3-128 signature.
- **Pending identity** is zero payload length plus an all-zero signature.
- **Plan** is a move-only point-in-time classification bound to one persisted library id and committed library revision.
- **Relink** rebinds an existing track and manifest row from an old URI to a new URI.

## Invariants

- Edited library metadata is authoritative after initial import; a changed-file scan refreshes technical properties without replacing curated metadata.
- A scan never admits an unsupported file into its plan.
- Only the runtime planner can construct an applicable plan; consumers may inspect its immutable items but cannot insert or rewrite them.
- Plan application accepts only the library id and revision captured by the planner, so a foreign, superseded, or already-consumed snapshot cannot mutate storage.
- One applied plan commits all successful content changes and the revision atomically.
- Runtime scan apply closes interactive admission before slow preparation and keeps it closed through publication and callback-owner finalization.
- Filesystem reads, media parsing, and fingerprinting hold no coordinator writer ownership or LMDB write transaction.
- Cancellation before commit leaves all track, manifest, identity, and relink state unchanged.
- A relink preserves `TrackId` and updates the track URI and manifest binding together or not at all.
- Automatic relinking requires one missing row and one new file with exactly equal non-pending audio identity.
- Identity backfill never commits a hash for a row or file whose live size or modification time changed after snapshot.
- Persisted manifest corruption stops plan construction or backfill with `CorruptData`; it is not a per-file `Error` item and no partial plan or later manifest row is delivered.
- Before scan apply mutates any item, every plan item that names an existing Track must still have valid hot and cold records; missing evidence is `CorruptData` and is never reinterpreted as a new Track.

## Plan classification

The planner recursively walks the configured music root, skips unsupported and non-regular entries, normalizes root-relative URIs, and compares supported regular files with the manifest.

| Classification | Meaning |
|---|---|
| `New` | Supported URI has no manifest row and is not unambiguously matched as a move. |
| `Changed` | URI exists but file size or modification time differs. |
| `Moved` | New URI is uniquely matched to one missing manifest row by audio identity. |
| `Missing` | Manifest URI has no corresponding supported file and was not matched as a move. |
| `Unchanged` | URI and file facts match the manifest. |
| `Error` | The item could not be inspected or classified. |

A missing root or root-level walk failure is a plan-building error.
Per-entry problems may appear as error items without erasing other classifications.
These item errors describe external filesystem, path-resolution, or media inspection failures.
Malformed persisted manifest keys or values instead fail the complete `buildPlan()` result at the first bad row, because continuing would make corrupt storage look like an ordinary missing file.
An entry that is present but cannot be resolved or inspected safely is `Error`, not also `Missing`; its existing manifest row remains unchanged when the plan is applied.
The planner reads the persisted library id, committed revision, and manifest from the same LMDB snapshot and stores that binding in the returned plan.

The planner performs URI matching before identity matching.
It groups missing rows with stored identities by payload length and signature.
A new file is hashed only when its payload length can match at least one missing row.
Ambiguous duplicate groups remain `Missing` plus `New` for explicit resolution.

## Plan application

Runtime application enters `ScanApply` maintenance, prepares plan items on a worker without writer ownership, then opens one coordinator mutation to revalidate prepared state and apply every successful content change atomically.
It reports updating and fingerprinting progress during preparation.
The runtime-private operation enforces `Created → Prepared → Revalidated → Applied/Terminal`; callers cannot skip final file revalidation or apply the same operation twice.

The two compositions share that state machine but not transaction ownership:

- live `LibraryTaskService` calls `prepare()`, then obtains the coordinator transaction and calls `apply()`;
- offline `run()` prepares first, acquires its own writable-library lease, revalidates, applies, and commits one isolated transaction.

There is no nullable or mode-switching transaction branch inside `apply()`.

After maintenance closes interactive admission, application validates the plan binding before reporting item progress, opening media, or fingerprinting.
The write transaction validates the same library id again and requires its newly allocated revision to immediately follow the plan revision before it touches track or manifest rows.
A single pre-mutation pass also validates the hot/cold evidence for every nonzero planned Track id.
A foreign binding returns `InvalidInput`; a superseded or replayed binding returns `Conflict`; both paths abort without a durable revision or content change.
Missing or invalid Track evidence returns `CorruptData` and aborts the staged revision plus every planned item; the `Changed` branch cannot fall through to `New`.

The transaction currently covers the complete prepared plan and has no item or byte bound.
This preserves whole-plan all-or-nothing behavior; writer hold time and rollback cost scale with the prepared plan.

- `New` parses metadata and technical properties, creates a track, and writes an available manifest row.
- `Changed` preserves curated metadata, refreshes technical properties, and refreshes file and identity facts.
- `Moved` rebuilds the existing track with the new URI and refreshed technical properties, removes the old manifest key, and writes the new key with the same track id.
- `Missing` preserves the previous identity and marks the manifest row missing.
- `Unchanged` performs no write.
- Item-level parse/open failures are counted and reported without claiming that item succeeded.

After preparation and immediately before opening the coordinator mutation, application fingerprints every moved destination again and compares it with both the prepared and planned identities.
A mismatch or a failure after relink processing begins aborts the complete transaction.
New and changed files do not receive an equivalent final stat check, and missing paths are not checked for reappearance.

Successful explicit relink derivation consumes an unresolved plan and produces one `Moved` item only when the selected `Missing` and `New` items carry the same non-pending planned identity.
The derived plan preserves the source library and revision binding, and live destination fingerprinting remains mandatory during apply.

The result carries the committed revision, inserted/mutated/relinked ids, missing count, and item-failure count; the relink count is `relinkedIds.size()` rather than independent state.
Coordinated and offline application report cancellation through `OperationCancelled`, so a successful result never also claims cancellation.
Only a successful commit makes those content counts observable.

## Deferred identity

`AudioIdentityPolicy::Eager` fingerprints new and changed files during apply.
`DeferNew` may write a newly imported available row with pending identity so metadata becomes visible quickly.
Changed and moved files remain eager, and a plan item that already carries a valid new-file identity reuses it.

Pending rows cannot participate in automatic or validated explicit relinking until backfill supplies identity.
Aobus never writes a guessed identity.

## Identity backfill

`AudioIdentityIndexer` processes bounded batches in three phases:

1. Under one outer `AudioIdentityBackfill` maintenance interval, snapshot available pending rows and their URI, size, and modification time in a read transaction without writer ownership.
2. Fingerprint files concurrently outside LMDB transactions; the default concurrency is `clamp(hardware_concurrency / 2, 2, 4)`.
3. Acquire one bounded coordinator mutation per serial write-back batch, re-read every row, and commit identities for rows still available, pending, and stat-equal.

Per-file failures are reported and counted without aborting the run; database failures fail the operation.
Manifest iteration validates each persisted row and returns `CorruptData` at the existing task boundary rather than skipping it or treating it as pending work.
Progress callbacks are serialized but may run on worker-pool threads.

Cancellation stops hashing at chunk boundaries, commits valid rows already completed in the current batch, leaves unfinished rows pending, and propagates `OperationCancelled` after callback-owner maintenance cleanup.
The successful result contains only completed, skipped, and per-item-failure counts; it never also represents cancellation.
Earlier committed batches remain durable, and the next run resumes from the pending manifest rows rather than reconstructing partial counts for the cancelled run.
Backfill changes only manifest identity; each effective batch publishes its committed revision with no track/list category rather than claiming a metadata mutation.

## Signature behavior

Identity hashing uses `utility::Xxh3Accumulator128` and is invariant to hashing chunk boundaries.
The signature is a local non-cryptographic identity aid, not a security boundary.
Pairing the 128-bit signature with payload length is the complete equality key.

## Failure and cancellation

Filesystem, mapping, tag parsing, media corruption, database, and resource-limit failures use `Result` or the per-item failure channel according to whether useful plan/application work can continue.
Safely detected malformed persisted manifest or Track evidence is operation-level `CorruptData`, not an external-item failure that permits continuation.
Any post-effect storage failure exits the complete scan transaction owner through `lmdb::TransactionFailure`; the outer boundary translates it only after the transaction has aborted.
Cancellation is cooperative during payload hashing and before commit.
The shared UIModel workflow distinguishes an up-to-date plan, an errors-only
plan, and an actionable plan before application. Frontends remain responsible
only for presenting issues and scheduling optional identity backfill after a
fast-bootstrap scan. Committed data reaches runtime replicas and projections
through `LibraryChanges`; workflow completion performs no independent refresh.

## Implementation map

- [`LibraryScan.h`](../../../../app/include/ao/rt/library/LibraryScan.h) and [`ScanPlan.h`](../../../../app/include/ao/rt/library/ScanPlan.h) define the shared scan surface.
- [`LibraryScan.cpp`](../../../../app/runtime/library/LibraryScan.cpp) owns planning and move matching.
- [`ScanApplyOperation.cpp`](../../../../app/runtime/library/ScanApplyOperation.cpp) owns the shared state machine, the self-contained offline `run()` composition, and transaction-scoped apply.
- [`LibraryTaskService.cpp`](../../../../app/runtime/library/LibraryTaskService.cpp) owns maintenance lifetime and prepare/apply worker composition.
- [`LibraryScanWorkflow.cpp`](../../../../app/uimodel/library/task/LibraryScanWorkflow.cpp) owns frontend-shared plan disposition, issue collection, identity policy, and build/apply orchestration.
- [`AudioIdentity.h`](../../../../include/ao/library/AudioIdentity.h) owns identity calculation.
- [`AudioIdentityIndexer.cpp`](../../../../app/runtime/library/AudioIdentityIndexer.cpp) owns concurrent backfill.

## Test map

- [`ScanPlanTest.cpp`](../../../../test/unit/runtime/library/ScanPlanTest.cpp) proves opacity, classifications, URI normalization, move identity, constrained explicit relink derivation and consumption, ambiguity, and errors.
- [`ScanApplyOperationTest.cpp`](../../../../test/unit/runtime/library/ScanApplyOperationTest.cpp) proves binding rejection, replay protection, prepared-file revalidation, atomic application, curated-metadata preservation, relinking, failures, progress, and cancellation.
- [`LibraryScanWorkflowTest.cpp`](../../../../test/unit/uimodel/library/task/LibraryScanWorkflowTest.cpp) proves frontend-shared plan disposition and mutation reporting.
- [`AudioIdentityIndexerTest.cpp`](../../../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp) proves concurrency, revalidation, cancellation, skip, and failure behavior.
- [`AudioIdentityTest.cpp`](../../../../test/unit/library/AudioIdentityTest.cpp) proves signature calculation and cancellation.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Library change publication](change-publication.md)
- [Supported audio files](../../../reference/media/audio-file.md)
