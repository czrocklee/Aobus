---
id: library.yaml-transfer
---
# Library YAML transfer

## Scope

This specification defines library YAML export and import behavior.
It owns mode semantics, baselines, payload scope, overlays, preview-bound authorization, atomicity, reports, and change publication.

The exact version 8 document shape is defined by the [library YAML format reference](../../reference/library/format/yaml.md).
Library ownership and the storage/change pipeline are defined by [library architecture](structure.md).
CLI flags and output rendering belong to the [CLI command reference](../../reference/cli/command.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../overview.md).
`LibraryYamlExporter`, `LibraryYamlImporter`, and `LibraryJobs` translate between a portable document and `ao::library::MusicLibrary`; YAML is not a physical storage format.
Interactive and CLI callers use `LibraryJobs` so a live-runtime commit consumes preview evidence rather than accepting a bare path.
The explicit `LibraryYamlImporter::*Offline` methods instead own an isolated writable-library lease and transaction; they do not enter runtime maintenance or publish through `LibraryChanges` and must not mutate a library attached to a live runtime.

## Terminology

- **Source library** is the library being exported.
- **Target library** is the library being imported into.
- **Payload mode** is the document's `export-mode` and controls payload scope and baseline reconstruction.
- **Import mode** is `restore` or `merge` and controls how the payload combines with target state.
- **File baseline** is a track builder loaded from the audio file named by a Library URI when that file is readable.
- **Merge baseline** is an existing target track matched by canonical manifest URI.
- **Import plan** is a move-only, one-shot authorization containing the prepared payload, preview report, exact source bytes, target library identity and revision, and runtime identity.
- **Present collection** means `tags`, `custom`, `covers`, or `credits` exists in a track record, including an explicitly empty sequence or map.

## Invariants

- One export observes metadata, tracks, lists, resources, dictionary values, and manifest facts through one read transaction.
- Version 8 uses the closed schema and explicit collection scope defined by the format reference.
- Every URI crossing YAML, manifest, Writer, or scan boundaries becomes a `LibraryUri`; playback, read-model, fingerprint, export/import baseline, and scan-apply access resolve it again beneath the weakly canonical root and reject escaping or unresolved symlinks. An absent root or ordinary missing suffix remains valid for first-run metadata restore.
- Import validates the complete document before applying any persistent mutation. In both restore and merge, every track record must name a supported audio file, as [manual creation](mutation.md#create-from-file) does; an unsupported extension rejects the whole document as `FormatRejected`. Supported-extension files may still be absent or unreadable for offline restoration.
- Track metadata, tags, custom keys and values, and List display text must be scalar-valid UTF-8; canonically decomposed input is accepted and normalized to NFC by core library preparation before persistence.
- List filter source is scalar-valid UTF-8 but remains byte-exact so URI literals retain filesystem identity.
- Library URI bytes retain their separate path identity and are never Unicode-normalized by transfer.
- One committed import applies its content through one write transaction and one library revision.
- Import never changes the target's `libraryId`; a document carries none.
- Preview runs the same mutation path in an uncommitted transaction and publishes no content change.
- Each live preview or apply operation acquires the import background-task lease, enters Maintenance through the library command lane, runs only generation-bound Maintenance commands, and exits Maintenance through that lane before its public Task completes.
- Revision settlement for a committed import precedes Maintenance workflow settlement; the final `Available` notification and its observers complete before the operation returns.
- A prepared plan can commit only against the exact source bytes and target runtime, library identity, and committed revision it previewed.
- A collection field that is present replaces its complete baseline collection; an omitted collection preserves its baseline.
- Restore scope is determined by payload mode, never inferred from omitted collections.
- A payload has at most one whole-file record for each canonical URI. Several records may share a URI only as one carrier's segments, with distinct ordinals and agreeing file facts and extent. Merge matches a whole-file Track by canonical URI and a segment by `(uri, ordinal)`. Payload track IDs exist only for intra-payload references.
- Lists in the payload are recreated with new target IDs and then have parents remapped.
- A List filter and saved order are independent: the filter determines local membership, while order references preserve rank only.
- Track preparation validates credit kinds, text, dates, counts, and complete-record bounds before interning dictionary text or creating cover resources. NFC interning can still fail exceptionally after staging begins; the root import transaction must abort on every post-effect failure, with no per-item recovery.
- A post-effect Track, manifest, List, identifier, or storage failure reaches the root transaction boundary, which aborts the whole import before returning the error; import never catches a private mutation marker to continue with another payload item.
- Only manifest point-read `NotFound` means an absent merge baseline or dangling URI reference; a post-open malformed row is an `AO_INVARIANT` fault and a non-miss native read failure is `AO_FATAL`, so neither becomes partial import or export output.

## State model

The application import path has two operations:

```text
prepare(path, import mode)
  -> enter sequenced Import Maintenance
  -> read exact source bytes
  -> parse and validate version 8
  -> prepare track/list data
  -> capture target runtime + library id + committed revision
  -> run exact preview in one generation-bound lane turn and abort
  -> exit Maintenance and deliver Available
  -> return LibraryImportPlan + ImportReport

explicit authorization
  -> enter a new sequenced Import Maintenance interval
  -> consume LibraryImportPlan
  -> recheck runtime + library id + committed revision
  -> reread and compare exact source bytes
  -> run prepared mutation path in one generation-bound lane turn
  -> commit once
  -> settle one published change set while still in Maintenance
  -> exit Maintenance and deliver Available
```

Rejecting or dropping a plan performs no persistent mutation.
A plan has no time-based expiry; its source and target bindings make stale plans unusable.

The synchronous offline importer is a separate lower-level capability and test surface.
It performs preparation, acquires its own writable-library lease, previews or commits one transaction, and returns the report directly.
It does not reuse a nullable branch inside the live operation and is not the frontend authorization boundary.

## Commands and transitions

### Export modes

| Payload mode | Metadata | Custom metadata | Tags | Covers | Technical and manifest facts | Lists |
|---|---|---|---|---|---|---|
| `delta` | Fields different from a readable file baseline; otherwise all non-empty fields. | Complete map when non-empty. | Complete sequence when non-empty. | Never emitted. | Omitted. | Included. |
| `metadata` | All non-empty scalar metadata and explicit Credits sequence, including empty. | Complete map when non-empty. | Complete sequence when non-empty. | Never emitted. | Omitted. | Included. |
| `full` | All non-empty scalar metadata and explicit Credits sequence, including empty. | Complete map when non-empty. | Complete sequence when non-empty. | Reference sequence, including empty, plus the `library.resources` table. | Included, including zero values. | Included. |
| `list-only` | No track records. | No track records. | No track records. | No track records. | No track records. | Included with URI rank references; a segment reference adds `ordinal`. |

No mode carries a cover byte; `full` carries each cover's digest, length, picture type, and order, and names each distinct cover once.
No mode is a copy of the files' tags either: a document carries only what the database holds, and a source field that the [source field mapping](../../reference/media/audio-file.md#source-field-mapping) does not import is never there.
Every mode that emits track records also emits `segment` for a segment Track, including `delta` and `metadata`. Without it those records would be duplicate URIs, and those modes otherwise omit the technical facts a restore needs to derive duration. The field shape is the [segment format](../../reference/library/format/yaml.md#segment).
A `full` export opens no audio file and needs none to succeed, because a descriptor is a database fact.
`delta` and `metadata` omit covers entirely: a mode whose consumer reconstructs the file baseline would otherwise overwrite the art that baseline just read with a sequence the exporting database may no longer agree with.

The exporter constructs the complete YAML tree before writing anything, then installs it under the [atomic file replacement](../persistence/atomic-replacement.md) contract; a destination below a directory that does not exist fails with `IoError` and creates nothing, because naming the directory is the caller's part.
The destination therefore holds its previous content, or nothing, until one complete document replaces it: an export that fails or is cancelled costs the user nothing, which matters because the path an export names is usually the backup it is replacing.
An export observes cancellation between records and installs no file when it stops; that is a cancelled task rather than an export failure.
For `delta`, a missing, unsupported, or unreadable audio file means no baseline and causes emission of all applicable current values.
A filesystem inspection error fails export with `IoError`.

### Restore

For `delta`, `metadata`, and `full`, restore clears tracks, manifest rows, and lists inside the import transaction before rebuilding them.
For `list-only`, restore preserves tracks and manifest rows and clears only lists.
Restored rows receive fresh ids above every id the database ever committed, so a reference saved against this database before the restore names no restored object; document ids only bind references inside the document.
A new database has committed no id, so its restored ids start at `1` and can equal the ids of the document's source or of another database.
Restore never changes the target's `libraryId`, so a restore into a new database leaves it distinguishable by id from the library the document came from.

Restore chooses a track baseline by payload mode:

- `full` starts from an empty track, applies payload values, and opens no audio file;
- `delta` starts from a readable file baseline when available;
- `metadata` may retain file technical properties and the file's current cover references, but clears file-derived curated metadata, tags, and custom metadata before applying the payload;
- when an optional file baseline cannot be opened or parsed, restore starts from an empty track.

Restore preparation does not take file size, modification time, audio identity, or status from the target manifest it later clears. [Track overlays](#track-overlays) own that facts baseline.

### Merge

Merge preserves target tracks and lists absent from the payload.
An imported whole-file record whose canonical URI matches a whole-file manifest row updates that track; an unmatched whole-file URI creates a track and manifest row.
A whole-file record meeting a carrier, or a carrier record meeting a whole-file row, rejects the document. Merge never converts one form into the other.

The existing target track is the merge baseline.
For `delta` and `metadata`, a readable source file refreshes technical properties of a whole-file Track; delta also supplies the file's cover references when the baseline has none.
Payload fields then overlay that baseline.
A segment Track keeps its sample rate, range, and derived duration across that refresh.

### Carrier tracks

`full` restore opens no audio file, including when the payload already supplies the carrier extent, sample rate, and file facts needed to rebuild the rows. It validates each carrier's ranges against `total-frames`, takes the hot sample rate from `segment`, and leaves physical truth to the next scan and to playback. The restored carrier's audio identity stays pending, and its status stays the builder default `Available`. That default is not an observation that the file is present.
`delta` and `metadata` read a present file for technical Track properties and omitted file size and modification time, not for an audio-identity fingerprint. A readable carrier whose exact extent or sample rate differs from `total-frames` or `segment.sample-rate` rejects the document. A missing or unreadable file does not invent those facts and does not by itself reject a structurally valid segment record.

Merge validates the final carrier, not only the document, and fails the whole import if that carrier would be invalid.
A payload carrier meeting a target carrier first requires the target's extent and sample rate, whether or not any ordinal matches.
It then matches by `(uri, ordinal)`, requires equal ranges for a matched segment, and may add an unmatched ordinal only when the final ranges stay inside the extent and do not overlap.
Merge never edits a stored range. File facts must agree across the payload records of one URI.
The same check runs again inside the write transaction against the complete prepare-time member set, so a member deleted or added after preparation, including an ordinal the payload does not touch, makes the import stale rather than committing a partial carrier. The offline importer reports that membership change as `InvalidState`. A live `LibraryJobs` apply still rejects any target revision change as `Conflict` before applying.

Merge does not match or update existing lists.
Every payload list is created as a new target list, after which parent IDs are remapped.

### Track overlays

Present metadata and technical scalar fields replace the corresponding baseline value.
Omitted scalar fields preserve it.

Collections use replacement semantics:

- omitted field: preserve the baseline collection;
- present non-empty field: replace the complete collection;
- present empty sequence or map: clear the complete collection.

A recognized codec token replaces the baseline codec; any other token rejects the payload.
Manifest file facts follow the import mode, not a single old-target baseline.
Merge starts from the existing target manifest row when the canonical URI matches. A carrier row keeps its audio identity and status. A whole-file row contributes its stored file size and modification time. The payload then overlays those facts.
Restore does not consult the target row it clears. It starts from the no-row baseline: a present `file-size` or `mtime` wins; an omitted permitted fact is seeded from the filesystem when that path is present; absent media leaves an omitted fact as a zero file size and no stored modification time. An unmatched URI uses that same no-row baseline in either mode.
Restore does not inherit audio identity or status from the old target, including a carrier, and it does not fingerprint the file. Identity stays pending: zero payload length and an all-zero signature. Status stays the builder default `Available`, which is not a claim that an absent file is physically available.
A present `file-size` field overrides the selected baseline.
A present `mtime` value overrides the modification-time fact: the map form stores its instant, and an explicit YAML null clears the stored modification time to absent; an omitted `mtime` preserves the selected baseline.

The performance-information fields follow the same overlay shape with two narrow rules.
A `recording-date` scalar holds its value. Empty scalar text is the clear form that stores the absence sentinel: a quoted empty scalar or a present empty plain scalar (`recording-date:`). The literal texts `null` and `~` are nonempty and are not that clear form. An omitted key preserves the baseline date. The [format reference](../../reference/library/format/yaml.md#recording-date) owns the scalar grammar.
A `credits` sequence replaces every credit kind together; explicitly empty clears all, and omitted preserves the selected baseline.
Merge into an existing track preserves its stored credits on omission in every mode, including delta; a media read refreshes technical facts without replacing that curated metadata baseline.
Delta creation/restore uses media credits when file reading succeeds, otherwise the empty-metadata fallback. Metadata creation/restore starts with empty credits even when media supplies technical facts; full creation/restore likewise has no implicit media credits.
The importer validates the complete date and every credit in document preflight through the [date grammar](../../reference/library/format/yaml.md#recording-date) and [closed credit schema and admission](../../reference/library/format/yaml.md#credits), before any durable effect.
No media reader populates a recording date, so a delta baseline's date is always absent.
Full/metadata exports always emit the credits sequence, including empty. Delta compares complete normalized credits against the selected media baseline or empty fallback, including kind, role, duplicates, and within-kind order; a difference emits the whole sequence, including a clear. An unadmissible media baseline compares as different rather than dropping stored credits.
List-only transfer never carries or changes track credits.

### Cover terminal state

Covers follow the same overlay rule as any other collection: a present `covers` sequence replaces the baseline's, and an absent one preserves it.
Only `full` may carry that key, so the mode decides the outcome:

| Payload mode | Restore | Merge |
|---|---|---|
| `full` | The document's reference graph, including which tracks share one cover. | The document's reference graph for the tracks it names. |
| `metadata` | The file's current art, which may differ from the exporting database's state when the file was retagged in between; no covers when the file cannot be read. | The target track's covers, unchanged. |
| `delta` | The file's current art; no covers when the file cannot be read. | The target's covers, filled from the file only when the target has none. |
| `list-only` | Untouched. | Untouched. |

A `full` import derives each `ResourceId` from the digest rather than reading one from the document, and writes descriptors only: no cover content is written, whatever the size of the collection.
A declared length fills a descriptor row that does not exist and never overwrites one a writer counted.

A restored library displays a cover on its first request when a surviving cache entry holds that digest, or when a referencing URI resolves to a readable file still carrying it; it displays no image, and rewrites no reference, when neither holds.
No rescan is required between a `full` restore and a cover appearing.

### Lists

Lists are created in payload order, then parent relationships are applied in a second pass so a child may precede its parent.
Saved order resolves payload IDs through tracks created or updated by this import and URI references through the target manifest.
A `uri` reference without `ordinal` matches only a whole-file Track. With `ordinal`, it matches that segment of the URI. Either miss is a dangling reference, not a match against another member.

An empty or omitted filter means the identity predicate.
A nonempty filter is imported byte-exactly even when it does not parse or compile under the current query grammar, because the database can already hold such text and the exporter writes it unchanged; rejecting it would leave that library's backup unrestorable.
The import counts such a filter, and the imported List exposes the same empty-membership expression error that the [track source](track-source.md) defines for invalid stored text.
Filter and order may coexist, and order references never add a track to List membership.
Known payload parent relationships must be self-free and acyclic before any list is written.
Every recreated list must fit the fixed-width core list layout; an oversized text field, track array, or combined offset rejects the import instead of truncating data.

Unresolved parent and track references are ignored and counted.
Saved order keeps the first resolved occurrence and preserves first-occurrence order, including ranks for tracks that are currently outside effective membership.

### Reports

Every import, preview, and plan returns an `ImportReport`:

| Field | Meaning |
|---|---|
| `payloadVersion` | Accepted interchange version; currently `8`. |
| `payloadMode` | `delta`, `metadata`, `full`, or `list-only`. |
| `targetScope` | `ImportTargetScope::Library` for track-bearing payloads or `ImportTargetScope::Lists` for `list-only`. |
| `tracksCreated` | Imported records that do not match a merge baseline. |
| `tracksUpdated` | Imported records that match an existing target track: a whole-file URI, or a carrier `(uri, ordinal)`. |
| `tracksDeleted` | Pre-restore tracks when scope clears tracks; otherwise `0`. |
| `listsCreated` | Payload lists created by the import. |
| `listsDeleted` | Pre-restore lists; otherwise `0`. |
| `danglingReferencesIgnored` | Unresolved parent, payload-track-ID, and manifest-URI references. |
| `invalidFiltersKept` | Imported nonempty List filters that do not parse or compile, kept byte-exactly. |

Counts describe processed matches and creations, not only byte-different records.
Preview and commit produce the same report when source bytes and target binding remain unchanged.

### Change publication

A committed live-runtime import publishes one `LibraryChangeSet` carrying the transaction revision.
Preview publishes nothing.

Restore publishes `libraryReset: true` and no incremental ID lists.
Merge publishes `libraryReset: false` with newly inserted tracks, matched tracks as mutated, and newly created lists as upserted.

## Failure and cancellation

File-read failures report `IoError`.
Malformed YAML, unsupported versions, closed-schema violations, invalid values, and unsafe URIs report `FormatRejected` as defined by the [format validation rules](../../reference/library/format/yaml.md#validation-rules).
Malformed UTF-8 in library text and custom keys that collide after NFC normalization are invalid values under that rule; neither is repaired with a replacement character.
Applying a plan to different source bytes, another runtime, another library identity, or another target revision reports `Conflict`.
Every apply attempt consumes the plan, including an attempt that returns a pre-commit error; replay reports `InvalidState` and a retry requires a fresh preview.

Any failure before commit leaves target content, metadata identity, and revision unchanged and publishes no content change.
Commit failure likewise publishes no change set.
The import transaction is one lexical owner with no nested item transactions: pre-effect validation may return a typed error, while a failure after any item has staged state unwinds that owner before translation.
Export iterator integrity faults likewise unwind without producing a partial YAML document or being translated through a private library exception in runtime.

After a durable live-runtime commit, revision-admission, publication-admission, or delivery failure follows [library change publication](change-publication.md#failure-and-lifetime): durable state is not rolled back or reported as a retryable import failure, and a live runtime terminates rather than exposing a recovery state.
Offline import has no runtime publication phase; its transaction commit result is its terminal outcome.

`LibraryJobs` honors cancellation on executor transitions.
Once synchronous transfer work begins it has no internal stop checkpoint; after a possible commit it returns to the callback executor without reinterpreting committed state as cancelled.
Before commit, cancellation aborts or prevents the Maintenance mutation and the workflow still completes its sequenced Maintenance exit while the runtime remains live.
After durable commit, the command must reach `Published` or coordinated-Closing retirement before cancellation can propagate; a durable import is never reported as rolled back.
The operation matrix belongs to [library task execution](task-execution.md#cancellation).

Version 8 currently defines no transfer-specific total-document byte budget beyond the exact field and core-storage limits in the format reference; a cover contributes a fixed-size row rather than its content.
No configurable prepared-memory ceiling, streaming path, or additional bounded-transfer proposal is currently defined.
Adding a limit must preserve the guarantee that the current exporter cannot produce a file the importer rejects solely for size.

## Persistence and versioning

Version 8 is a portable interchange format, not the physical database format.
Restore and merge always write current `MusicLibrary` records.
An accepted version-8 document may use a canonically decomposed spelling, but export from physical database version 10 emits NFC because that is the current library admission invariant.
Only the exact [version-8 schema](../../reference/library/format/yaml.md) is accepted. Alternative schemas labeled version 8 are unsupported, without aliases, a dual reader, or layout autodetection.
Empty scalar text, including a present empty plain scalar, is the date's clear form.
Libraries admitted by earlier version-5 importers could retain unsupported-extension rows; that version-5 history stays visible across the version-5 to version-6 transition, and its export consequences remain policy: export does not filter those rows, while current whole-document admission rejects their reimport, with no migration, purge, or grandfathering performed.
A fresh version-10 library cannot gain such a row, because current import admission rejects the document and no older physical version can be opened.
The importer accepts no earlier interchange version, including version 7, and provides no migration or legacy-restore path.
A version-3 document's embedded cover bytes are therefore never read: the import fails and changes nothing.
No build can re-export an old physical library into a document the current importer accepts, because the current build opens only physical version 10 while older builds emit interchange versions this importer rejects; recovering such a library means scanning the music files it describes.

## Frontend observations

CLI, GTK, and WinUI use the same plan-producing runtime operation and one-shot apply operation.
No frontend may commit a restore from a bare path or reinterpret scope, counts, matching, or publication.

### Building an interactive transfer surface

GTK and WinUI build their export selectors from one UIModel definition ordered as `delta`, `metadata`, `full`, and `list-only`.
Both default to `full`.
An invalid native selector index remains an adapter concern: GTK conservatively falls back to `metadata`, while WinUI returns no mode and does not reinterpret the selection.

Restore confirmation is also shared presentation policy.
Given the structured `ImportReport`, UIModel selects the Library- or Lists-scoped action and formats the complete version, scope, counts, and ignored-reference summary.
The displayed payload mode argument is the report's stable machine token (`delta`, `metadata`, `full`, or `list-only`), not the localized export-selector label; display text is never parsed back into transfer identity.

GTK prepares a restore plan after file selection, presents its version, payload mode, scope, counts, ignored references, and kept invalid List filters, and applies only after an explicit positive response.
Closing or rejecting the confirmation drops the plan.

WinUI exposes all four export modes through its Windows save picker and offers `merge` or `restore` before opening its Windows file picker.
Merge is the default and applies the prepared plan without a destructive confirmation.
Restore presents the shared report's version, payload mode, target scope, track/List create-update-delete counts, ignored references, and kept invalid List filters in a native confirmation; cancellation or rejection drops the plan and changes nothing.
Import preparation, apply, and export publish their coarse file-named phases through the shared task-progress channel used by the Windows activity surface.

CLI defaults import to `merge`.
`--mode restore --dry-run` prepares and prints a plan report without committing; a non-dry-run restore additionally requires `--confirm-destructive-restore`.
The apply step still revalidates source and target evidence, so the flag cannot authorize a changed preview.

## Implementation map

- [`LibraryTransfer.h`](../../../app/include/ao/rt/library/LibraryTransfer.h) defines the public transfer modes and reports.
- [`LibraryTransferPresentation.h`](../../../app/include/ao/uimodel/library/presentation/LibraryTransferPresentation.h) and [`LibraryTransferPresentation.cpp`](../../../app/uimodel/library/presentation/LibraryTransferPresentation.cpp) own shared selector ordering, the Full export default, and restore-confirmation presentation.
- Source-private [`LibraryYamlExporter`](../../../app/runtime/library/LibraryYamlExporter.h) and [`LibraryYamlExporter.cpp`](../../../app/runtime/library/LibraryYamlExporter.cpp) implement export modes and baselines.
- Source-private [`LibraryYamlImporter`](../../../app/runtime/library/LibraryYamlImporter.h) and [`LibraryYamlImporter.cpp`](../../../app/runtime/library/LibraryYamlImporter.cpp) implement strict parsing and prepared mutation behavior.
- [`LibraryImportPlan`](../../../app/include/ao/rt/library/LibraryImportPlan.h) and [`LibraryJobs`](../../../app/include/ao/rt/library/LibraryJobs.h) define preview-bound application authorization.
- [`LibraryWriteLane`](../../../app/runtime/library/LibraryWriteLane.h) owns sequenced Maintenance entry, generation-bound preview/apply turns, revision settlement, and workflow exit.
- [`LibraryUri`](../../../include/ao/library/LibraryUri.h) defines canonical root-relative path evidence.
- [`LibraryChanges`](../../../app/include/ao/rt/library/LibraryChanges.h) defines published change values.
- [`LibraryTransferCoordinator`](../../../app/windows-winui/library/LibraryTransferCoordinator.h) owns Windows pickers, native confirmation, notifications, and window-lifetime cancellation; [`LibraryTransferAdapter`](../../../app/windows-winui/include/ao/winui/library/LibraryTransferAdapter.h) maps stable selector rows and restore-only destructive admission without WinRT types.

## Test map

- [`LibraryExportImportTest.cpp`](../../../test/unit/runtime/library/LibraryExportImportTest.cpp) proves mode baselines, overlays, reports, and preview equivalence.
- [`LibraryExportImportDeltaTest.cpp`](../../../test/unit/runtime/library/LibraryExportImportDeltaTest.cpp) proves delta behavior, library id retention, rollback, and change sets.
- [`LibraryExportImportCoverArtTest.cpp`](../../../test/unit/runtime/library/LibraryExportImportCoverArtTest.cpp) proves the resource table, reference closure, export determinism, and each mode's cover terminal state.
- [`LibraryExportImportListTest.cpp`](../../../test/unit/runtime/library/LibraryExportImportListTest.cpp) proves list-only transfer, references, remapping, and dangling counts.
- [`LibraryYamlSchemaTest.cpp`](../../../test/unit/runtime/library/LibraryYamlSchemaTest.cpp) proves closed-schema, scope, enum, URI, duplicate-key, list-semantic, and storage-limit rejection.
- [`CueYamlTest.cpp`](../../../test/unit/runtime/CueYamlTest.cpp) proves segment emission, offline restore, contradictory duration and sample rate, and atomic carrier merge.
- [`LibraryExportImportErrorTest.cpp`](../../../test/unit/runtime/library/LibraryExportImportErrorTest.cpp) proves scalar rejection and transactional rollback.
- [`LibraryYamlImporterTest.cpp`](../../../test/unit/runtime/library/LibraryYamlImporterTest.cpp) proves recording-date and credit preflight admission and whole-document rejection.
- [`LibraryYamlExporterTest.cpp`](../../../test/unit/runtime/library/LibraryYamlExporterTest.cpp) proves recording-date and credits emission and delta baselines.
- [`LibraryYamlPerformanceMetadataTest.cpp`](../../../test/unit/runtime/library/LibraryYamlPerformanceMetadataTest.cpp) proves date-precision and list round trips, clear forms, and merge overlay preservation.
- [`LibraryJobsTest.cpp`](../../../test/unit/runtime/library/LibraryJobsTest.cpp) proves source/target binding, one-shot plans, cancellation before maintenance, and mandatory callback completion after commit.
- [`LibraryTransferPresentationTest.cpp`](../../../test/unit/uimodel/library/presentation/LibraryTransferPresentationTest.cpp) proves export-option order and the Full default, both restore scopes, complete report arguments, and stable payload-mode tokens.
- [`LibraryTransferAdapterTest.cpp`](../../../test/unit/winui/library/LibraryTransferAdapterTest.cpp) proves every WinUI selector mapping and restore-only destructive admission.
- [`LibraryImportExportWorkflowTest.cpp`](../../../test/unit/linux-gtk/portal/LibraryImportExportWorkflowTest.cpp) proves confirmation precedes GTK mutation.
- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) proves CLI preview and explicit restore confirmation.

## Related documents

- [Decision 0015: sequence live-runtime library writes](../../decision/0015-sequence-live-runtime-library-writes.md)
- [Library YAML format reference](../../reference/library/format/yaml.md)
- [Library architecture](structure.md)
- [Outcome channel specification](../failure/outcome-channel.md)
- [Error value reference](../failure/error.md)
