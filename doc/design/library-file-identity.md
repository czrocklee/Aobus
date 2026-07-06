# Library File Identity

Status: Current behavior. Aobus stores audio payload signatures in the file
manifest, may temporarily defer signatures for first-bootstrap imports,
automatically relinks unambiguous in-root file moves when identity is present,
and exposes explicit CLI paths for unresolved relinks and pending identity
backfill.

## Ownership

The library database owns edited metadata. File tags initialize track metadata
on first import, but changed-file rescans refresh technical audio properties
only. A file identity mechanism must therefore identify the audio payload, not
the whole tagged file, so external retagging does not break identity.

`lib/library` is the primitive storage and schema layer: persisted stores,
views, builders, and LMDB-backed library access. Runtime owns scan and
reconcile application semantics. The scan planner/applier compose library
primitives, but they are runtime-private `ao::rt` workflows, not public
`ao::library` primitives. The frontend-shared scan entry point is
`ao::rt::LibraryScan`; `ao::rt::AudioIdentityIndexer` and scan are both
runtime-owned library workflows. As a result, `ao_library` alone no longer
contains a writer that reconciles manifest state with track state; those
reconciliation writers live in runtime.

## Audio Payload

`tag::TagFile::audioPayload()` returns the encoded audio byte range borrowed
from the mapped file:

- FLAC: bytes after the final metadata block.
- MP4: the single top-level `mdat` payload.
- MP3: bytes from the first MPEG frame after ID3v2 to before trailing
  ID3v1/APEv2.
- WAV: the RIFF `data` chunk payload.

The range excludes known tag and non-audio container regions. FLAC currently
treats every byte after the final metadata block as payload and rejects an
empty payload. WAV excludes RIFF headers, `fmt `, `LIST/INFO`, `fact`, and
other non-`data` chunks, and rejects an empty `data` chunk. MP3 skips junk
between ID3v2 and the first MPEG frame; encoder padding inside MPEG frames
remains part of the encoded payload. Callers hash the returned span; no decode
is required.

## Signature

The current content signature is `utility::Hash128` produced by XXH3-128
(xxHash) through the `utility::Xxh3Accumulator128` wrapper. This is a
non-cryptographic 128-bit signature for local content identity, not a security
boundary. Aobus stores payload length with the 128-bit signature so accidental
collisions require both the byte count and the full signature to match; 128-bit
is used instead of 64-bit to keep the birthday collision margin comfortably
outside realistic local-library sizes without adding meaningful storage cost.

The manifest stores the signature bytes in the XXH128 canonical serialization
(`XXH128_canonicalFromHash`, defined big-endian), so the stored representation
is platform independent. Incremental hashing is chunk-boundary invariant:
hashing one span or the same bytes split across `Xxh3Accumulator128::mix()`
calls produces the same signature. Identity readers hash audio payloads through
`Xxh3Accumulator128` in bounded chunks so callers receive fingerprinting
progress and cancellation is honored while hashing large files. xxHash is an
implementation detail of the `ao/utility/Xxh3.h` wrapper; library-layer headers
never include `<xxhash.h>`.

## Manifest Format

`FileManifestHeader` is 48 bytes with 4-byte alignment:

- `TrackId`
- whole-file size
- whole-file mtime
- audio payload length
- 128-bit audio payload signature
- file status

The library format version is `4`. Aobus does not carry compatibility
migration code for older metadata headers; opening a library whose stored
version differs from the current version reports `CorruptData`. Development
databases must be reset and rescanned after a format bump. There is no separate
signature-algorithm version field; a future signature algorithm change must
bump the library format version, causing existing development databases to be
rejected until reset and rescanned.

Rows with zero audio payload length and the all-zero signature have pending
audio identity. This is valid for available rows after a fast bootstrap scan:
track metadata and file stat fields are visible immediately, while payload
identity is filled later. Aobus never writes a guessed identity; pending is
preferred over stale or unverifiable identity.

## Scan Matching And Writes

Runtime-private `ao::rt::ScanPlanApplier` normally writes payload length and
signature for New and Changed classifications. Fast bootstrap mode may defer
New-item fingerprinting; those manifest rows are written as available with file
size and mtime but a pending audio identity. Changed and Moved items remain
eager because they refresh or validate existing file bindings. It reports
`Updating` progress before applying each item and `Fingerprinting` progress
while hashing an item's audio payload. Missing rows preserve their previous
signature because they are rebuilt from the existing manifest view and only the
status changes.

After runtime-private `ao::rt::ScanPlanBuilder`'s URI-first pass, missing
manifest entries with stored audio identity are grouped by payload length and
signature. New files are opened only when at least one missing identity exists;
their payload length is read first, and the payload is hashed only when that
length can match a missing row.

A group is auto-relinked only when it contains exactly one missing row and
exactly one new row with the same payload length and signature. The new scan
item becomes `ScanClassification::Moved`, carrying the old URI and old
`TrackId`, and the matched missing item is removed from the plan. Duplicate
audio groups remain as `Missing` plus `New` so a human can resolve them later.
They are not auto-relinked in later stages unless the ambiguity disappears or a
manual `lib relink --from ... --to ...` command resolves one pair.

Missing rows with pending identity cannot participate in automatic moved-file
matching or explicit relink validation until backfill completes. The CLI relink
path tells users to run `aobus lib fingerprint --pending` when a requested
missing row has no audio signature.

`ao::rt::ScanPlanApplier` applies a moved item by rebuilding the existing track
from the database record, setting the cold URI to the new root-relative URI,
refreshing technical audio properties from the file, removing the old manifest
row, and writing the new manifest row with the existing `TrackId`.
Before rebinding, it re-fingerprints the live file and compares that identity to
the planned identity; if the file changed between scan planning and apply, the
relink fails and the write transaction is not committed.

Successful moved items increment `ScanApplyResult::relinkedCount`; successfully
marked missing rows increment `ScanApplyResult::missingCount`. The CLI plain
scan output and GTK scan completion notification use those counts to report
`Relinked N moved file(s)` and `N missing file(s) need review` after the
transaction commits.

If scan application is cancelled, `ao::rt::ScanPlanApplier` stops between
fingerprinting chunks and returns a cancelled result before committing its write
transaction. LMDB aborts the uncommitted transaction, so partially processed
tracks, manifest rows, signatures, and relinks are not persisted.

Moved-item failures that occur after relink processing starts also abort the
write transaction. A relink must update the track URI and manifest row together
or not at all.

## Audio Identity Indexing

`library::readAudioIdentity()` computes the encoded audio payload identity.
Runtime `AudioIdentityIndexer` scans the manifest for available rows whose
identity is pending and fills them in three phases per bounded batch:

1. **Snapshot.** A plain LMDB read transaction collects the batch's pending
   rows (URI, path, size, mtime). No mutation lock is held: the write-back
   phase revalidates every row, so a stale snapshot cannot commit stale state.
2. **Concurrent fingerprint.** Rows are hashed outside any LMDB transaction
   and without any lock by a bounded set of workers on the shared async worker
   pool; the default worker count is `clamp(hardware_concurrency / 2, 2, 4)`
   because hashing is disk-bound and the pool is shared with the rest of the
   app. The coordinator awaits the workers through `async::whenAll` and holds
   no pool thread while suspended, so indexing cannot starve a small pool.
   Before hashing, a worker verifies the live file is still a supported
   regular audio file whose size/mtime match the manifest snapshot, and
   verifies size/mtime again after hashing. Per-file failures are reported and
   counted without aborting the run; database failures are fatal.
3. **Serial write-back.** The hashed rows of a batch are written back in a
   single write transaction, so the commit cost is amortized across the batch.
   The constructor-provided mutation mutex is locked only around this phase:
   `LibraryTasks` passes its mutation mutex, so scans, imports, and exports
   block on backfill only during these short write windows, never while files
   are being hashed. Inside the transaction each row is reread, rebuilt with
   `FileManifestBuilder::fromView()`, and written with
   `FileManifestStore::Writer::put()` only if it is still available, still
   pending, and still has the same size/mtime; a row that changed mid-batch is
   skipped without aborting the rest of the batch. This prevents a stale hash
   from being committed when the file or manifest changes while indexing is
   running.

Cancellation stops workers between hashing chunks, flushes rows already hashed
in the current batch (they are valid completed work), and returns a cancelled
result; rows not yet hashed or written stay pending.

Indexing updates manifest identity only. Runtime and GTK completion paths do not
emit track-mutation notifications for indexing because track rows and displayed
metadata do not change.

## Explicit Relink CLI

`aobus lib relink` is the manual recovery path for duplicate-content or
otherwise unresolved `Missing` plus `New` groups. With no `--from`/`--to`
arguments it lists unresolved missing rows, new files, and exact audio-identity
candidates. `ao::rt::ScanPlanBuilder` fingerprints only `New` files whose
payload length matches at least one missing row; files with different lengths
cannot be exact identity candidates.

`aobus lib relink --from <old-uri> --to <new-uri>` validates that both sides are
currently unresolved and that the audio payload length and signature match. The
CLI apply path builds a one-item `Moved` scan plan and sends it through runtime
`LibraryScan`, which uses runtime-private `ScanPlanApplier`; manual relinks
preserve the same dual-rebind invariant as automatic relinks.
`--dry-run` validates and reports the planned binding without mutating the
library.

`aobus scan --defer-fingerprint` imports new track metadata immediately and
leaves new manifest rows with pending audio identity. `aobus lib fingerprint
--pending` fills pending identities and reports fingerprinted, skipped, and
failed row counts. Skipped rows remain pending for a future normal scan or
indexing run.
