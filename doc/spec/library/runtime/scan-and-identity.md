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
The frontend-shared surface is `app/include/ao/rt/library/LibraryScan.h`, `ScanPlan.h`, and `LibraryTaskService.h`; planning, reconciliation, and backfill live in `app/runtime/library/`, while encoded-payload extraction and manifest storage remain core-library facilities.
`LibraryScan` is read-only plan construction.
Committing application is a runtime-private coordinator operation; focused storage tests may use its explicitly offline composition.

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
- Runtime scan apply closes interactive admission before slow preparation and keeps it closed through publication and completion.
- Filesystem reads, media parsing, and fingerprinting hold no coordinator writer ownership or LMDB write transaction.
- Cancellation before commit leaves all track, manifest, identity, and relink state unchanged.
- A relink preserves `TrackId` and updates the track URI and manifest binding together or not at all.
- Automatic relinking requires one missing row and one new file with exactly equal non-pending audio identity.
- Identity backfill never commits a hash for a row or file whose live size or modification time changed after snapshot.

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

After maintenance closes interactive admission, application validates the plan binding before reporting item progress, opening media, or fingerprinting.
The write transaction validates the same library id again and requires its newly allocated revision to immediately follow the plan revision before it touches track or manifest rows.
A foreign binding returns `InvalidInput`; a superseded or replayed binding returns `Conflict`; both paths abort without a durable revision or content change.

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
[RFC 0004](../../../rfc/0004-scan-file-revalidation.md) proposes that narrow revalidation.

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
Progress callbacks are serialized but may run on worker-pool threads.

Cancellation stops hashing at chunk boundaries, commits valid rows already completed in the current batch, leaves unfinished rows pending, and returns `cancelled = true`.
Backfill changes only manifest identity; each effective batch publishes its committed revision with no track/list category rather than claiming a metadata mutation.

## Signature behavior

Identity hashing uses `utility::Xxh3Accumulator128` and is invariant to hashing chunk boundaries.
The signature is a local non-cryptographic identity aid, not a security boundary.
Pairing the 128-bit signature with payload length is the complete equality key.

## Failure and cancellation

Filesystem, mapping, tag parsing, media corruption, database, and resource-limit failures use `Result` or the per-item failure channel according to whether useful plan/application work can continue.
Cancellation is cooperative during payload hashing and before commit.

## Implementation map

- [`LibraryScan.h`](../../../../app/include/ao/rt/library/LibraryScan.h) and [`ScanPlan.h`](../../../../app/include/ao/rt/library/ScanPlan.h) define the shared scan surface.
- [`ScanPlanBuilder.cpp`](../../../../app/runtime/library/ScanPlanBuilder.cpp) owns planning and move matching.
- [`ScanApplyOperation.cpp`](../../../../app/runtime/library/ScanApplyOperation.cpp) owns transactional application.
- [`LibraryTaskService.cpp`](../../../../app/runtime/library/LibraryTaskService.cpp) owns maintenance lifetime and prepare/apply worker composition.
- [`AudioIdentity.h`](../../../../include/ao/library/AudioIdentity.h) owns identity calculation.
- [`AudioIdentityIndexer.cpp`](../../../../app/runtime/library/AudioIdentityIndexer.cpp) owns concurrent backfill.

## Test map

- [`ScanPlanBuilderTest.cpp`](../../../../test/unit/runtime/library/ScanPlanBuilderTest.cpp) proves opacity, classifications, URI normalization, move identity, constrained explicit relink derivation and consumption, ambiguity, and errors.
- [`ScanApplyOperationTest.cpp`](../../../../test/unit/runtime/library/ScanApplyOperationTest.cpp) proves binding rejection, replay protection, prepared-file revalidation, atomic application, curated-metadata preservation, relinking, failures, progress, and cancellation.
- [`AudioIdentityIndexerTest.cpp`](../../../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp) proves concurrency, revalidation, cancellation, skip, and failure behavior.
- [`AudioIdentityTest.cpp`](../../../../test/unit/library/AudioIdentityTest.cpp) proves signature calculation and cancellation.

## Related documents

- [Library architecture](../../../architecture/library.md)
- [Library change publication](change-publication.md)
- [Supported audio files](../../../reference/media/audio-file.md)
- [RFC 0004: scan file revalidation](../../../rfc/0004-scan-file-revalidation.md)
