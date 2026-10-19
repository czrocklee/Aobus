---
id: library.scan-identity
---
# Library scan and audio identity

## Scope

This specification defines `LibraryScan`, scan plans, plan application, encoded-audio identity, automatic move relinking, and pending-identity backfill.
It owns filesystem-to-library reconciliation behavior rather than the physical manifest record.

Supported containers, imported tags, and encoded payload ranges belong to the [supported audio files reference](../../reference/media/audio-file.md).
CUE grammar, encoding, and same-name matching belong to the [CUE sheet reference](../../reference/media/cue-sheet.md).
Manifest keys and fields belong to the [library database reference](../../reference/library/storage/database.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../overview.md).
The runtime surface is `app/include/ao/rt/library/LibraryScan.h`, `ScanPlan.h`, and `LibraryJobs.h`; planning, reconciliation, and backfill live in `app/runtime/library/`, while encoded-payload extraction and manifest storage remain core-library facilities.
`app/include/ao/uimodel/library/task/LibraryScanOutcome.h` exposes the frontend-shared `runLibraryScanAsync` operation and final outcome. Its private implementation owns build/classify/apply orchestration, including the eager versus fast-bootstrap identity strategy.
`LibraryScan` is read-only plan construction.
Live committing application is a runtime-private coordinator operation.
The runtime-private `ScanApplyOperation::run()` is the distinct offline composition used by focused storage workflows and tests.

## Terminology

- **Manifest URI** is the normalized music-root-relative key of one audio file. A whole-file row names one Track. A carrier row names every segment Track of that file.
- **Carrier** is a FLAC or WAV file a CUE sheet divides into segment Tracks. A carrier has only segment Tracks.
- **Ordinal** is a segment Track's CUE `TRACK` number. It identifies the segment across rescans and is not the curated track number.
- **Sheet evidence** is the file's candidate sets at both name precedences, each name with size, modification time, and an XXH3 digest of the sheet bytes. An empty set records absence.
- **Audio identity** is encoded-audio payload length plus its XXH3-128 signature.
- **Pending identity** is zero payload length plus an all-zero signature.
- **Plan** is a move-only point-in-time classification bound to one persisted library id and carrying its planner revision as provenance.
- **Relink** rebinds an existing track and manifest row from an old URI to a new URI.
- **Modification time** is the manifest's portable Unix/POSIX instant defined by the [library database reference](../../reference/library/storage/database.md); a row without one stores absence, which is not a clock value.

## Invariants

- Edited library metadata is authoritative after initial import; a changed-file scan refreshes technical properties without replacing curated metadata, including exact RecordingDate and canonical Credits, with kind, role, within-kind order, and duplicates.
- Scan and pending-audio-identity backfill never populate or overwrite RecordingDate or Credits for an existing Track; audio identity backfill is not metadata backfill.
- A source field that the [source field mapping](../../reference/media/audio-file.md#source-field-mapping) does not import never enters the library: no scan or import that reads the file stores it.
- An embedded cover is a scan fact rather than curated metadata: the file is the authority on which covers a track has, and a scan is the only writer of that reference set.
- A scan never admits an unsupported file into its plan.
- A scan never deletes a Track, never changes a stored segment range, and never converts a whole-file Track into a carrier or the reverse. Re-splitting a file is the user's action: remove its Tracks and scan again.
- Sheet text is read only for Tracks being created. A text-only sheet edit writes nothing, as a tag edit does not replace a changed file's curated metadata.
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
- A modification time crosses filesystem, manifest, plan, and backfill boundaries as one portable Unix/POSIX instant: the planner converts each inspected file's native clock to that scale, a conversion the native clock cannot express is a per-item failure, and comparisons include stored absence, so an absent stored time never equals a file's present instant.
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
| `MissingSegments` | An unchanged carrier's sheet has ordinals the carrier lacks, and every stored segment still equals the sheet's segment of that ordinal. |
| `Unchanged` | URI and file facts match the manifest, and no missing ordinal is being created. |
| `Error` | The item could not be inspected or classified, its directory's sheets could not be read and it is not `Unchanged`, or CUE binding refused to import a new file. |
| `Notice` | A report-only entry: a readable CUE sheet with two or more audio tracks names no supported file. It names no library file and is never applied. |

A missing root or a walk that cannot start or advance to the next entry is a
plan-building error; the walk iteration cannot continue past a failed advance.
Per-entry problems may appear as error items without erasing other classifications.
These item errors describe external filesystem, path-resolution, or media inspection failures.
A malformed manifest cannot enter a live planner through supported storage: open rejects persisted corruption, while a later point-read or iterator breach aborts through the fatal facility because continuing would make invalid storage look like an ordinary missing file.
An entry that is present but cannot be resolved or inspected safely is `Error`, not also `Missing`; its existing manifest row remains unchanged when the plan is applied.
A directory, a directory symlink, or a supported audio file whose root-relative path forms no library URI, for example one longer than 1,976 bytes after normalization, is an `Error` item labeled with its full relative path; the message gives the byte count when length is the cause and says when a directory's contents were not scanned.
A file of an unsupported format needs no URI, so the walk skips it even when its path forms none.
The planner reads the persisted library id, committed revision, and manifest from one short LMDB snapshot, copies every manifest row into owned plan-building state, and closes the transaction before walking the filesystem.
Cancellation is checked while copying the manifest, traversing entries, reporting progress, adding missing rows, matching moves, and hashing candidate payloads.

The planner performs URI matching before identity matching.
It groups missing rows with stored identities by payload length and signature.
A new file is hashed only when its payload length can match at least one missing row.
Ambiguous duplicate groups remain `Missing` plus `New` for explicit resolution.
Identity matching runs before CUE binding, so a destination that binding alone would leave unimported is still a move when the identities match.
Planning can still refuse a matched move whose destination it cannot inspect, such as one whose folder's sheets or whose media cannot be read. That destination is `Error` without the source's evidence, and the source stays `Missing`, so application marks its row and a later scan can pair the files again.

## CUE binding

Every external sheet read resolves the candidate as a `LibraryUri` under the configured music root before opening it.
That includes scan planning, add-file, preparation, and final revalidation, and it includes a directory entry that is a symlink.
`LibraryUri::resolveUnder` follows an existing symlink and rejects a target outside the root or an unresolved symlink.
A symlink to a regular file inside the root may be read.
Resolving before the read does not promise that the inode is unchanged between the check and the completed read.
The directory itself must also resolve under that root; a directory outside the root is not listed.
The listing keeps every `.cue` entry, in any ASCII case, whatever its file type, and every retained candidate is resolved and read on every scan.
An entry that does not resolve to a regular file, such as a FIFO or a directory, is not opened; it is an unreadable candidate.
An external sheet must not be read beyond 4,194,304 bytes (4 MiB).
The limit applies to the size observed before the read and to growth observed while the bytes are read.
A sheet over the limit is unreadable.
A sheet applies only to audio files with its name, as the [CUE sheet reference](../../reference/media/cue-sheet.md#which-file-a-sheet-names) defines. The `FILE` name is not used.
The nonempty candidate set with the higher precedence decides. More than one sheet in that set leaves the file undetermined, and the error names the sheets.
A same-name candidate blocks the embedded FLAC `CUESHEET` comment and blocks importing the file whole, even when that candidate is unreadable, empty, outside the root, not a regular file, or over the read limit.
There is no fallback.
Only a nonempty FLAC comment is an embedded binding source; Opus text from shared comment extraction is not. Two different nonempty comments are ambiguous, like two same-name sheets, and leave the file undetermined. The binary FLAC cue-sheet block is not a sheet. The [CUE sheet reference](../../reference/media/cue-sheet.md#embedded-text) owns that eligibility rule.

For each supported file the planner derives one binding state after the file classification above:

| Binding | Meaning |
|---|---|
| Bound | The deciding sheet is readable, has two or more audio tracks, and the file is FLAC or WAV with an exact frame count. |
| Unbound | The file has no sheet, or its sheet has one audio track. |
| Undetermined | The deciding sheet is unreadable or malformed, several sheets share that precedence, or the file cannot be a carrier. |

| Library state | Bound | Unbound | Undetermined |
|---|---|---|---|
| New file | Create segment Tracks. | Create a whole-file Track. | `Error`; the file is not imported until the sheet is fixed. |
| Whole-file Track | Keep the Track. Report that it was imported as a whole file and must be removed to be split. | Unchanged as a binding. | Keep the Track and report the sheet. |
| Carrier | Create ordinals the carrier lacks when every stored segment still matches; otherwise keep every member and range and report the mismatch. | Keep the members and report that no sheet divides the file. | Keep the members and report the sheet. |

"Unchanged" in that table means no Track is created and no member or range changes. It does not withhold a status, file-fact, technical, or cover refresh.
A carrier whose exact extent or sample rate differs from the stored ones is `Error` and stays unchanged, including when the file would otherwise be `Changed`.
A `Moved` or relinked file keeps its source form whatever the binding at the destination: a carrier keeps every member id, ordinal, and range and relinks them together, and a whole-file Track stays whole.
The applied result reports every affected member Track id but counts the carrier once as a relinked physical file.
A carrier moves only when the destination's exact extent and sample rate equal the stored ones. Otherwise the destination is `Error` and is not imported, and the source stays `Missing`.
A binding report accompanies the file classification instead of replacing it, except for the `Error` cases above and for `MissingSegments`.
Such a report, like a `Notice` entry, is a notice: it is shown and logged as a warning, but it neither blocks its item nor fails the scan or explicit relink. Only an `Error` item is a failure.
Missing ordinals are created only for a carrier whose file facts are unchanged, so a moved or refreshed carrier gains them on the next scan.
An ordinal the carrier lacks is created again, including one the user deleted. That is `MissingSegments`, not `New`, so move matching never treats it as a new file.

A sheet that names no supported file is ignored, except that a readable sheet with two or more audio tracks is a `Notice` entry saying that no supported audio file has its name.
The planner reads the sheets of every directory that holds a planned item's resolved path or a sheet the walk listed.
A file symlink resolves to its target, whose directory the walk need not have listed.
A directory whose sheets cannot be read, including one that vanished after the walk, makes each of its items `Error`; other directories still plan, and the plan is not abandoned.
An `Unchanged` item applies nothing, so it keeps its classification and reports the read failure as a notice with the unreadable-sheet diagnostic.
A file reached through a symlink into a folder that can be traversed but not listed therefore cannot be imported until that folder can be listed; each scan reports it as `Error`, beside the walk's own error for the folder.
Directories and their reports are ordered by path, so neither the report order nor the notification excerpt depends on hashing.
A non-UTF-8 sheet is unreadable, and its error asks the user to save it as UTF-8.

The planner's snapshot reads the segment blocks and stored extent of every carrier row.
Those stored facts are not the file's current capability, extent, or sample rate.
A carrier scan always opens the physical file through the media reader and uses that reader's codec, exact frame count, and sample rate, including when the carrier is unchanged and when an external sheet is present.
A stored member's codec or rate is not a substitute, even when every member records the same value.
This is not a requirement that members share a codec, and it does not change the stored format.
A layout is the ordered ordinals and starts, plus any data-track end, resolved against the reader's exact extent.
That comparison does not decode the audio payload.
Apply preparation opens the media reader again for a new, changed, moved, or missing-segment item, and does not decode the payload for the extent comparison.
An unchanged item is not opened again during preparation.
An unchanged whole-file Track with no external candidate is not opened during planning either: its embedded source was checked on admission, and unchanged file facts preserve that result.
New, changed, or moved files still inspect their media, and a newly added external candidate is checked even for an unchanged whole-file Track.
Full YAML restore still trusts the payload's structural carrier facts and does not open or decode the audio file.
That boundary belongs to [YAML transfer](yaml-transfer.md#carrier-tracks).

New segment Tracks take title from the track `TITLE`, artist from the track `PERFORMER`, else the sheet's, else the file's, and album and album artist from the sheet `TITLE` and `PERFORMER`, else the file's.
Composer comes from `SONGWRITER` in that same track-then-sheet-then-file order.
An empty sheet value, such as `TITLE ""`, counts as absent, so the next source in that order applies.
Genre, year, and disc numbers come from the track `REM` fields, else the sheet's, else the file's. A `REM` number is the leading unsigned integer when one is present; otherwise the file's value is kept. That parse is not the audio-file Year grammar.
Track number is the ordinal, and track total is the sheet's audio-track count for that file.
Work, movement, Credits, custom metadata, and covers come from the carrier and apply to every new segment. Curated data on an existing member stays authoritative.

## Plan application

Runtime application acquires the exclusive `ScanApply` background-task lease, prepares plan items on a worker without transaction ownership, then submits one coordinator background command to admit prepared state and apply every successful content change atomically.
Authoring availability remains `Available` during preparation.
Once the background command is queued or active, later Track/List authoring returns non-terminal `Busy` and ordinary mutation/preview commands return `ResourceBusy` instead of waiting behind the scan; an interactive command accepted first retains its FIFO turn.
It reports updating and fingerprinting progress during preparation.
Preparation reads each directory's sheets once and shares that snapshot among its items, including any directory-read failure. Final filesystem revalidation uses a separate directory snapshot and re-reads the sheets; it never admits an item using the preparation cache alone.
Each of those reads resolves candidates under the configured root and applies the external size limit before using the bytes.
Media preparation resolves an actionable item's URI once, and final filesystem proof resolves each prepared item once. In each phase the resolved canonical path must match the plan's physical-path provenance, and that same admitted path supplies the media reads and CUE directory. A changed resolution is an ordinary stale skip, or a failure that immediately aborts the whole application for `Moved`.
Reported failures use the CUE-binding stage only for a typed CUE diagnostic; unrelated walk and inspection failures retain the scan stage.
The runtime-private operation enforces `Created → Prepared → Revalidated → Applied/Terminal`; callers cannot skip final file revalidation or apply the same operation twice.

The two compositions share that state machine but not transaction ownership:

- live `LibraryJobs` calls `prepare()`, submits one background command, revalidates files during that command's active pre-transaction turn, then opens the transaction and calls `apply()`;
- offline `run()` prepares first, acquires its own writable-library lease, revalidates, applies, and commits one isolated transaction.

There is no nullable or mode-switching transaction branch inside `apply()`.

Application validates the persisted library id before reporting item progress, opening media, or fingerprinting, and validates that id again in the write transaction.
The planner revision remains diagnostic provenance and is not a transaction-admission gate.
A foreign binding returns `InvalidInput` without a durable revision or content change.

Immediately after the write transaction opens and before any item mutates storage, one pass admits actionable items against live database evidence:

- `New` requires the destination manifest URI to remain absent.
- `Changed` requires the same manifest URI, Track id, status, file size, and modification time, plus a live Track still bound to that URI. Curated Track fields and independently backfilled manifest identity are not admission evidence; apply re-reads the live complete Track and merges into it. For a carrier, the same pass also requires the stored extent, member ids, ordinals, ranges, and sample rate.
- `MissingSegments` requires that same carrier evidence. A member deleted or added after planning makes the item stale, not an invariant breach.
- `Missing` requires the same manifest URI, Track id, status, file facts, and Track binding. Independently completed identity is preserved from the live manifest and is not admission evidence. A carrier is marked missing as one row.
- `Moved` requires that complete source evidence under `oldUri`, including carrier membership when the source is a carrier, and an absent destination manifest.

A stale `New`, `Changed`, `MissingSegments`, or `Missing` item increments the stale count and is skipped without entering the per-item failure channel. Stale `Moved` evidence reports failure and aborts the complete application, matching explicit relink's stricter identity-preserving semantics.
Because this pass completes before the first mutation and LMDB serializes the writer, the logical writer's later Track/manifest binding invariants describe internal breaches rather than ordinary user races.

The transaction covers the complete admitted plan and has no item or byte bound.
This preserves whole-plan all-or-nothing behavior; writer hold time and rollback cost scale with the prepared plan.
There is no nested LMDB transaction, item savepoint, or partial batch commit.

- `New` parses metadata and technical properties, maps the public source-ordered Credits into the builder, validates text and bounds, and persists NFC text in canonical kind order through the logical Track writer together with the available manifest row. RecordingDate remains absent because there is no file-tag mapping for it. A bound file instead creates its segment Tracks through `createCarrier` and publishes every created id.
- `MissingSegments` adds only the new members through `addCarrierMembers`. It rewrites neither the existing members nor the row's identity, status, file facts, or extent, and it does not hash the file again.
- `Changed` preserves curated metadata, including exact recording-date precision and credit kinds, within-kind order, names, duplicates, and optional roles, replaces the track's cover references with the set the new file carries, then replaces Track data and file/identity facts through one logical operation. A carrier keeps every member's sample rate, range, and derived duration; bitrate, channels, codec, bit depth, covers, and file facts may refresh when the extent and rate still match.
Every carrier member is validated before the first write. A member the writer would reject is reported at the `serialize` stage and skips a changed carrier, or aborts the complete application for a moved one, as it would a whole-file Track.
- `Moved` first requires the stored Track URI to equal the plan's old URI, then rebuilds the existing track with the new URI, refreshed technical properties, and the destination file's cover references, and uses the logical relink operation to replace the manifest key while preserving the Track id; a mismatch reports the item failure and aborts the complete scan transaction. A carrier uses `relinkCarrier`, so every member moves together and no range changes.
- `Missing` preserves the previous identity and uses the manifest-only logical update to mark the row missing. A carrier's segments are missing together.
- `Unchanged` performs no write. The desktop summary still reports a binding notice on that item; apply does not count it as a failure. The CLI dry-run item list omits an `Unchanged` item only when it carries no diagnostic; a diagnostic-bearing unchanged item stays in that list under its file classification. That emission limit is recorded in the [command reference](../../reference/cli/command.md). Apply preparation progress is separate from the dry-run item list and reports every item, including a diagnostic-free `Unchanged` item.
- Item-level parse/open failures are counted and reported without claiming that item succeeded.
- Item-level Track validation failures, including malformed UTF-8, invalid structured credit input, or post-NFC/aggregate size overflow, are reported at the `serialize` stage and leave that new item absent.

Preparation snapshots own the raw credit names, roles, kinds, duplicates, and source order without normalizing or admitting them.
Reconstructed builders validate and canonicalize credits at Track admission: invalid new-item text fails that item without preventing valid siblings from committing.
Changed and moved items reconstruct the live complete Track before replacing technical/file facts and covers; newly parsed Credits never silently replace existing curation, even when stored credits or individual kinds are empty.
Malformed credit text in a changed or moved file therefore does not reject the item merely because that unused metadata was copied during preparation.
New imports use explicitly mapped credit kinds; `PERFORMER` never implies Soloist, and Ensemble fallback is resolved whole-file before library admission.
RecordingDate is not inferred from generic Year, `DATE`, `YEAR`, `TDRC`, album text, or audio identity.
No database migration, scalar/list conversion, or automatic metadata backfill accompanies ordinary scanning.
An explicit refresh/population workflow for existing tracks would be a separate contract, not a side effect of ordinary scans.

After preparation, once the background command owns the active lane turn and immediately before opening its write transaction, application re-stats every prepared new, changed, or moved file and requires its planned size and modification time, checks that every missing path remains absent, and fingerprints every moved destination again against both the prepared and planned identities.
It also lists each such item's directory again, derives both sheet-candidate sets, and re-reads each present sheet, comparing names, sizes, modification times, and XXH3 digests with the plan evidence.
A sheet added after planning under any case spelling, or edited without a size or time change, is therefore a stale skip rather than a bypass of an undetermined binding. One listing serves every item in a directory.
Stale new, changed, missing, or missing-segment filesystem evidence increments the stale count and skips that item without rewriting current facts. Path-resolution, permission, and other inspection failures remain per-item failures.
A moved mismatch, including a sheet set that changed after planning, aborts the complete application.

Successful explicit relink derivation consumes an unresolved plan and produces one `Moved` item only when the selected `Missing` source and destination carry the same non-pending planned identity, and, when the source is a carrier, the destination's inspected exact extent and sample rate equal the stored ones.
A `New` destination is admitted unless its diagnostic is a carrier-facts mismatch. An `Error` destination is admitted only when it has no stored manifest evidence, a present modification time, a nonempty planned path, a non-pending identity, and a CUE-binding diagnostic of invalid encoding, an unreadable or malformed sheet, ambiguous sheets, an unsupported carrier, or a layout mismatch.
Derivation keeps the source manifest evidence and classifies that stored form against the destination binding without another sheet read. A source-form diagnostic can therefore accompany the committed relink as a notice, not an item failure.
The derived plan preserves the source library binding, planner revision provenance, and missing-row evidence, and live destination fingerprinting remains mandatory during apply.

A scan writes descriptors and never cover content: preparation hashes each embedded picture, counts its bytes, and replaces the borrowed payload with an observed descriptor before any write transaction opens.
The Resource writer consumes that counted evidence without hashing again and may correct a previously declared length for the same digest.
A picture longer than `UINT32_MAX` is never truncated to fit a descriptor: preparation fails the complete scan rather than only that item.
No container this project reads can carry one, because FLAC and ID3 picture lengths are themselves 32-bit.
A file whose art was removed leaves the track with no cover reference, and the descriptors that reference set named stay in the store, because rows are never deleted.
Retagging a file back to its earlier art reuses the existing row, since equal content has one digest.
The scan report gains no cover-specific field: a track whose file is gone is counted `missing`, and a track whose art changed is counted `changed`, which replaces the stale reference rather than leaving something to report.

The result carries the committed revision, inserted ids, mutated ids, every affected member id in `relinkedIds`, the physical file count in `relinkedFileCount`, the missing count, the ordinary stale-item count, and the item-failure count.
One carrier contributes every member id and increments `relinkedFileCount` once.
Live publication appends `relinkedIds` to the mutated-track set, and workflow mutation detection uses those ids. Neither treats the physical file count as track identity.
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
A carrier is one pending row, so backfill hashes that file once and writes that one row.
It never adds recording dates or credits, nor infers either value from a matching hash or another Track.

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
plan, and an actionable plan before application. It counts `MissingSegments` with new files.
It counts `Error` items as failures and every other reported item as a notice, including one attached to an otherwise unchanged file.
Notices alone leave a plan up to date or applicable, and they are logged once as warnings because no apply pass reports them.
Frontends remain responsible
only for presenting issues and scheduling optional identity backfill after a
fast-bootstrap scan. Committed data reaches runtime replicas and projections
through `LibraryChanges`; workflow completion performs no independent refresh.
Created segment Tracks publish like other created Tracks. A scan deletes no Track, so it publishes no deletion of its own.

## Implementation map

- [`LibraryScan.h`](../../../app/include/ao/rt/library/LibraryScan.h) and [`ScanPlan.h`](../../../app/include/ao/rt/library/ScanPlan.h) define the shared scan surface.
- [`LibraryScan.cpp`](../../../app/runtime/library/LibraryScan.cpp) owns planning and move matching.
- [`CueBinding.cpp`](../../../app/runtime/library/CueBinding.cpp) owns sheet discovery, binding states, and new-segment metadata mapping.
- [`ScanApplyOperation.cpp`](../../../app/runtime/library/ScanApplyOperation.cpp) owns the shared state machine, the self-contained offline `run()` composition, and transaction-scoped apply.
- [`TrackWriter.h`](../../../include/ao/library/TrackWriter.h) owns the logical create, replace, relink, and manifest-only operations used during apply.
- [`LibraryJobs.cpp`](../../../app/runtime/library/LibraryJobs.cpp) owns background-task lifetime and prepare/apply worker composition.
- [`LibraryWriteLane.cpp`](../../../app/runtime/library/LibraryWriteLane.cpp) owns background-task exclusion, command-lane admission, active-turn revalidation placement, and publication settlement.
- [`LibraryScanWorkflow.cpp`](../../../app/uimodel/library/task/LibraryScanWorkflow.cpp) owns frontend-shared plan disposition, issue collection, identity policy, and build/apply orchestration.
- [`AudioIdentity.h`](../../../include/ao/library/AudioIdentity.h) owns identity calculation.
- [`FileTimestamp.h`](../../../include/ao/FileTimestamp.h) owns the portable modification-time instant, and [`FileTimeConversion.h`](../../../include/ao/library/FileTimeConversion.h) owns the native filesystem-clock conversion and last-write-time read.
- [`AudioIdentityIndexer.cpp`](../../../app/runtime/library/AudioIdentityIndexer.cpp) owns concurrent backfill.

## Test map

- [`ScanPlanTest.cpp`](../../../test/unit/runtime/library/ScanPlanTest.cpp) proves opacity, classifications, URI normalization, move identity, constrained explicit relink derivation and consumption, ambiguity, and errors, including entries whose paths form no URI.
- [`CueScanTest.cpp`](../../../test/unit/runtime/library/CueScanTest.cpp) proves same-name precedence, case-variant candidates, UTF-8 rejection, missing-ordinal recreation, and that a broken or newly added sheet deletes or rewrites nothing that exists.
- [`ScanApplyOperationTest.cpp`](../../../test/unit/runtime/library/ScanApplyOperationTest.cpp) proves library-id binding, stale-new replay protection, atomic application, curated-metadata preservation, cover replacement on `Changed` and `Moved` items, descriptor retention, relinking, failures, progress, and cancellation.
- [`ScanWorkMetadataTest.cpp`](../../../test/unit/runtime/library/ScanWorkMetadataTest.cpp) distinguishes work-source precedence on new imports from preservation of file-derived and manually curated work on changed-file scans.
- [`ScanPerformanceMetadataTest.cpp`](../../../test/unit/runtime/library/ScanPerformanceMetadataTest.cpp) protects new credit import and preservation of complete curated Credits and independent RecordingDate on rescans.
- [`ScanApplyFilesystemRevalidationTest.cpp`](../../../test/unit/runtime/library/ScanApplyFilesystemRevalidationTest.cpp) proves final new, changed, and missing filesystem checks.
- [`ScanApplyDatabaseAdmissionTest.cpp`](../../../test/unit/runtime/library/ScanApplyDatabaseAdmissionTest.cpp) proves commit-time item evidence, concurrent curation merge, deleted-Track containment, replacement safety, and moved-plan rollback.
- [`TrackBuilderSnapshotTest.cpp`](../../../test/unit/runtime/library/TrackBuilderSnapshotTest.cpp) proves raw credit-text ownership through source expiry and move, validation/canonicalization at reconstructed-builder admission, and conversion of scan cover bytes to counted observed descriptors before mutation.
- [`LibraryScanWorkflowTest.cpp`](../../../test/unit/uimodel/library/task/LibraryScanWorkflowTest.cpp) proves frontend-shared plan disposition and mutation reporting.
- [`AudioIdentityIndexerTest.cpp`](../../../test/unit/runtime/library/AudioIdentityIndexerTest.cpp) proves concurrency, revalidation, cancellation, skip, and failure behavior.
- [`AudioIdentityTest.cpp`](../../../test/unit/library/AudioIdentityTest.cpp) proves signature calculation and cancellation.

## Related documents

- [Decision 0015: sequence live-runtime library writes](../../decision/0015-sequence-live-runtime-library-writes.md)
- [Cover-art delivery](../resource/cover-art-delivery.md)
- [Decision 0010: never write to an audio file](../../decision/0010-never-write-to-audio-files.md)
- [Decision 0014: admit scan plans by item evidence](../../decision/0014-admit-scan-plans-by-item-evidence.md)
- [Library architecture](structure.md)
- [Library change publication](change-publication.md)
- [Supported audio files](../../reference/media/audio-file.md)
- [CUE sheet](../../reference/media/cue-sheet.md)
