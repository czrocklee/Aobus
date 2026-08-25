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
- **Plan** is a move-only point-in-time classification bound to one persisted library id and carrying its planner revision as provenance.
- **Relink** rebinds an existing track and manifest row from an old URI to a new URI.

## Invariants

- Edited library metadata is authoritative after initial import; a changed-file scan refreshes technical properties without replacing curated metadata.
- An embedded cover is a scan fact rather than curated metadata: the file is the authority on which covers a track has, and a scan is the only writer of that reference set.
- A scan never admits an unsupported file into its plan.
- Only the runtime planner can construct an applicable plan; consumers may inspect its immutable items but cannot insert or rewrite them.
- Plan application accepts only the library id captured by the planner and admits each actionable item against its own filesystem and database evidence.
- One applied plan commits all admitted content changes and the revision atomically; stale ordinary items are skipped, not partially committed in separate transactions.
- Runtime scan apply holds one background-task lease through preparation and publication. The lease excludes scan, backfill, and import preparation from each other without closing interactive authoring admission.
- Filesystem reads, media parsing, and fingerprinting hold no LMDB write transaction; final scan revalidation runs in the active pre-transaction lane phase.
- Plan construction owns a copy of the manifest snapshot and releases its LMDB read transaction before filesystem traversal, file I/O, or hashing.
- Media decoders may translate their declared legacy encodings, but library admission never repairs malformed declared UTF-8: a new file with invalid metadata becomes a per-item failure and no Track, manifest, dictionary row, or replacement character is committed.
- Cancellation before commit leaves all track, manifest, identity, and relink state unchanged.
- A relink preserves `TrackId` and updates the track URI and manifest binding together or not at all.
- Automatic relinking requires one missing row and one new file with exactly equal non-pending audio identity.
- Identity backfill never commits a hash for a row or file whose live size or modification time changed after snapshot.
- Persisted manifest corruption present at open rejects the library with `CorruptData`; a post-open manifest integrity breach aborts through the fatal facility and delivers no partial plan or later row.
- Before scan apply mutates any item, one transaction-internal preflight checks every actionable item's live manifest and Track evidence. Ordinary concurrent deletion or replacement becomes a stale-item skip; a later loss of already-admitted evidence inside that same write transaction remains an invariant breach.

## Plan classification

The planner recursively walks the configured music root, skips unsupported and non-regular entries, normalizes root-relative URIs, and compares supported regular files with the manifest.

| Classification | Meaning |
|---|---|
| `New` | Supported URI has no manifest row and is not unambiguously matched as a move. |
| `Changed` | URI exists but file size or modification time differs, or a present file restores a manifest row whose status is `Missing`. |
| `Moved` | New URI is uniquely matched to one missing manifest row by audio identity. |
| `Missing` | Manifest URI has no corresponding supported file and was not matched as a move. |
| `Unchanged` | URI and file facts match the manifest. |
| `Error` | The item could not be inspected or classified. |

A missing root or root-level walk failure is a plan-building error.
Per-entry problems may appear as error items without erasing other classifications.
These item errors describe external filesystem, path-resolution, or media inspection failures.
A malformed manifest cannot enter a live planner through supported storage: open rejects persisted corruption, while a later point-read or iterator breach aborts through the fatal facility because continuing would make invalid storage look like an ordinary missing file.
An entry that is present but cannot be resolved or inspected safely is `Error`, not also `Missing`; its existing manifest row remains unchanged when the plan is applied.
The planner reads the persisted library id, committed revision, and manifest from one short LMDB snapshot, copies every manifest row into owned plan-building state, and closes the transaction before walking the filesystem.
Cancellation is checked while copying the manifest, traversing entries, reporting progress, adding missing rows, matching moves, and hashing candidate payloads.

The planner performs URI matching before identity matching.
It groups missing rows with stored identities by payload length and signature.
A new file is hashed only when its payload length can match at least one missing row.
Ambiguous duplicate groups remain `Missing` plus `New` for explicit resolution.

## Plan application

Runtime application acquires the exclusive `ScanApply` background-task lease, prepares plan items on a worker without transaction ownership, then submits one coordinator background command to admit prepared state and apply every successful content change atomically.
Authoring availability remains `Available` during preparation.
Once the background command is queued or active, later Track/List authoring returns non-terminal `Busy` and ordinary mutation/preview commands return `ResourceBusy` instead of waiting behind the scan; an interactive command accepted first retains its FIFO turn.
It reports updating and fingerprinting progress during preparation.
The runtime-private operation enforces `Created → Prepared → Revalidated → Applied/Terminal`; callers cannot skip final file revalidation or apply the same operation twice.

The two compositions share that state machine but not transaction ownership:

- live `LibraryTaskService` calls `prepare()`, submits one background command, revalidates files during that command's active pre-transaction turn, then opens the transaction and calls `apply()`;
- offline `run()` prepares first, acquires its own writable-library lease, revalidates, applies, and commits one isolated transaction.

There is no nullable or mode-switching transaction branch inside `apply()`.

Application validates the persisted library id before reporting item progress, opening media, or fingerprinting, and validates that id again in the write transaction.
The planner revision remains diagnostic provenance and is not a transaction-admission gate.
A foreign binding returns `InvalidInput` without a durable revision or content change.

Immediately after the write transaction opens and before any item mutates storage, one pass admits actionable items against live database evidence:

- `New` requires the destination manifest URI to remain absent.
- `Changed` requires the same manifest URI, Track id, status, file size, and modification time, plus a live Track still bound to that URI. Curated Track fields and independently backfilled manifest identity are not admission evidence; apply re-reads the live complete Track and merges into it.
- `Missing` requires the same manifest URI, Track id, status, file facts, and Track binding. Independently completed identity is preserved from the live manifest and is not admission evidence.
- `Moved` requires that complete source evidence under `oldUri` and an absent destination manifest.

A stale `New`, `Changed`, or `Missing` item increments the stale count and is skipped without entering the per-item failure channel. Stale `Moved` evidence reports failure and aborts the complete application, matching explicit relink's stricter identity-preserving semantics.
Because this pass completes before the first mutation and LMDB serializes the writer, the logical writer's later Track/manifest binding invariants describe internal breaches rather than ordinary user races.

The transaction covers the complete admitted plan and has no item or byte bound.
This preserves whole-plan all-or-nothing behavior; writer hold time and rollback cost scale with the prepared plan.
There is no nested LMDB transaction, item savepoint, or partial batch commit.

- `New` parses metadata and technical properties, validates and NFC-normalizes admitted text, then asks the logical Track writer to create the track and available manifest row together.
- `Changed` preserves curated metadata, replaces the track's cover references with the set the new file carries, then replaces Track data and file/identity facts through one logical operation.
- `Moved` first requires the stored Track URI to equal the plan's old URI, then rebuilds the existing track with the new URI, refreshed technical properties, and the destination file's cover references, and uses the logical relink operation to replace the manifest key while preserving the Track id; a mismatch reports the item failure and aborts the complete scan transaction.
- `Missing` preserves the previous identity and uses the manifest-only logical update to mark the row missing.
- `Unchanged` performs no write.
- Item-level parse/open failures are counted and reported without claiming that item succeeded.
- Item-level Track validation failures, including malformed UTF-8 or post-NFC size overflow, are reported at the `serialize` stage and leave that new item absent.

After preparation, once the background command owns the active lane turn and immediately before opening its write transaction, application re-stats every prepared new, changed, or moved file and requires its planned size and modification time, checks that every missing path remains absent, and fingerprints every moved destination again against both the prepared and planned identities.
Stale new, changed, or missing filesystem evidence increments the stale count and skips that item without rewriting current facts. Path-resolution, permission, and other inspection failures remain per-item failures.
A moved mismatch aborts the complete application.

Successful explicit relink derivation consumes an unresolved plan and produces one `Moved` item only when the selected `Missing` and `New` items carry the same non-pending planned identity.
The derived plan preserves the source library binding, planner revision provenance, and missing-row evidence, and live destination fingerprinting remains mandatory during apply.

A scan writes descriptors and never cover content: preparation hashes each embedded picture, counts its bytes, and replaces the borrowed payload with an observed descriptor before any write transaction opens.
The Resource writer consumes that counted evidence without hashing again and may correct a previously declared length for the same digest.
A picture longer than `UINT32_MAX` is never truncated to fit a descriptor: preparation fails the complete scan rather than only that item.
No container this project reads can carry one, because FLAC and ID3 picture lengths are themselves 32-bit.
A file whose art was removed leaves the track with no cover reference, and the descriptors that reference set named stay in the store, because rows are never deleted.
Retagging a file back to its earlier art reuses the existing row, since equal content has one digest.
The scan report gains no cover-specific field: a track whose file is gone is counted `missing`, and a track whose art changed is counted `changed`, which replaces the stale reference rather than leaving something to report.

The result carries the committed revision, inserted/mutated/relinked ids, missing count, ordinary stale-item count, and item-failure count; the relink count is `relinkedIds.size()` rather than independent state.
Coordinated and offline application report cancellation through `OperationCancelled`, so a successful result never also claims cancellation.
Only a successful commit makes those content counts observable.

## Deferred identity

`AudioIdentityPolicy::Eager` fingerprints new and changed files during preparation.
`DeferNew` may write a newly imported available row with pending identity so metadata becomes visible quickly.
Changed and moved files remain eager, and a plan item that already carries a valid new-file identity reuses it.

Pending rows cannot participate in automatic or validated explicit relinking until backfill supplies identity.
Aobus never writes a guessed identity.

## Identity backfill

`AudioIdentityIndexer` processes bounded batches in three phases:

1. Under one outer `AudioIdentityBackfill` background-task lease, snapshot available pending rows and their URI, size, and modification time in a read transaction without a write-lane turn.
2. Fingerprint files concurrently outside LMDB transactions; the default concurrency is `clamp(hardware_concurrency / 2, 2, 4)`.
3. Acquire one bounded background mutation per serial write-back batch, re-read every row, and commit identities for rows still available, pending, and stat-equal.

The lease prevents another scan, backfill, or import preparation from overlapping the run, but it emits no `Maintenance` availability transition. Interactive authoring remains enabled between write-back batches and reports non-terminal `Busy` only while a bounded background batch is queued or active; ordinary commands use `ResourceBusy`.

Per-file failures are reported and counted without aborting the run; recoverable database failures fail the operation.
Manifest iteration trusts the open gate and aborts through `AO_INVARIANT` on a later row-integrity breach rather than skipping it, treating it as pending work, or translating a private mechanism in runtime.
Progress callbacks are serialized but may run on worker-pool threads.

Cancellation stops hashing at chunk boundaries, commits valid rows already completed and submitted in the current batch, leaves unfinished rows pending, and propagates `OperationCancelled` after callback-owner lease cleanup.
The successful result contains only completed, skipped, and per-item-failure counts; it never also represents cancellation.
Earlier committed batches remain durable, and the next run resumes from the pending manifest rows rather than reconstructing partial counts for the cancelled run.
Backfill changes only manifest identity; each effective batch publishes its committed revision with no track/list category rather than claiming a metadata mutation.

## Signature behavior

Identity hashing uses `utility::Xxh3Accumulator128` and is invariant to hashing chunk boundaries.
The signature is a local non-cryptographic identity aid, not a security boundary.
Pairing the 128-bit signature with payload length is the complete equality key.

## Failure and cancellation

Filesystem, mapping, tag parsing, media corruption, database, and resource-limit failures use `Result` or the per-item failure channel according to whether useful plan/application work can continue.
Malformed declared UTF-8 from a media tag uses that per-item channel; the library boundary neither guesses another encoding nor inserts U+FFFD.
Malformed persisted manifest or Track evidence discovered during library open returns
open-level `CorruptData`, not an external-item failure that permits continuation.
Commit-time evidence that differs from the plan is ordinary staleness and follows the item-admission rules above.
After admission, a Store or cross-Store integrity breach is an `AO_INVARIANT` fault; a
non-miss native cursor fault is `AO_FATAL`.
Any post-effect storage failure reaches the scan's root `WriteTransaction::apply()` boundary; that owner aborts the complete scan transaction before returning the carried `Error`.
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
- [`TrackWriter.h`](../../../../include/ao/library/TrackWriter.h) owns the logical create, replace, relink, and manifest-only operations used during apply.
- [`LibraryTaskService.cpp`](../../../../app/runtime/library/LibraryTaskService.cpp) owns background-task lifetime and prepare/apply worker composition.
- [`LibraryMutationService.cpp`](../../../../app/runtime/library/LibraryMutationService.cpp) owns background-task exclusion, command-lane admission, active-turn revalidation placement, and publication settlement.
- [`LibraryScanWorkflow.cpp`](../../../../app/uimodel/library/task/LibraryScanWorkflow.cpp) owns frontend-shared plan disposition, issue collection, identity policy, and build/apply orchestration.
- [`AudioIdentity.h`](../../../../include/ao/library/AudioIdentity.h) owns identity calculation.
- [`AudioIdentityIndexer.cpp`](../../../../app/runtime/library/AudioIdentityIndexer.cpp) owns concurrent backfill.

## Test map

- [`ScanPlanTest.cpp`](../../../../test/unit/runtime/library/ScanPlanTest.cpp) proves opacity, classifications, URI normalization, move identity, constrained explicit relink derivation and consumption, ambiguity, and errors.
- [`ScanApplyOperationTest.cpp`](../../../../test/unit/runtime/library/ScanApplyOperationTest.cpp) proves library-id binding, stale-new replay protection, atomic application, curated-metadata preservation, cover replacement on `Changed` and `Moved` items, descriptor retention, relinking, failures, progress, and cancellation.
- [`ScanApplyFilesystemRevalidationTest.cpp`](../../../../test/unit/runtime/library/ScanApplyFilesystemRevalidationTest.cpp) proves final new, changed, and missing filesystem checks.
- [`ScanApplyDatabaseAdmissionTest.cpp`](../../../../test/unit/runtime/library/ScanApplyDatabaseAdmissionTest.cpp) proves commit-time item evidence, concurrent curation merge, deleted-Track containment, replacement safety, and moved-plan rollback.
- [`TrackBuilderSnapshotTest.cpp`](../../../../test/unit/runtime/library/TrackBuilderSnapshotTest.cpp) proves that scan cover bytes become counted observed descriptors before mutation.
- [`LibraryScanWorkflowTest.cpp`](../../../../test/unit/uimodel/library/task/LibraryScanWorkflowTest.cpp) proves frontend-shared plan disposition and mutation reporting.
- [`AudioIdentityIndexerTest.cpp`](../../../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp) proves concurrency, revalidation, cancellation, skip, and failure behavior.
- [`AudioIdentityTest.cpp`](../../../../test/unit/library/AudioIdentityTest.cpp) proves signature calculation and cancellation.

## Related documents

- [Decision 0015: sequence live-runtime library writes](../../../decision/0015-sequence-live-runtime-library-writes.md)
- [Cover-art delivery](../../resource/cover-art-delivery.md)
- [Decision 0010: never write to an audio file](../../../decision/0010-never-write-to-audio-files.md)
- [Decision 0014: admit scan plans by item evidence](../../../decision/0014-admit-scan-plans-by-item-evidence.md)
- [Library architecture](../../../architecture/library.md)
- [Library change publication](change-publication.md)
- [Supported audio files](../../../reference/media/audio-file.md)
