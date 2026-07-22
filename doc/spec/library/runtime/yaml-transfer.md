---
id: library.yaml-transfer
type: spec
status: current
domain: library
summary: Defines strict export, restore, merge, preview authorization, reporting, and publication for library YAML transfers.
---
# Library YAML transfer

## Scope

This specification defines library YAML export and import behavior.
It owns mode semantics, baselines, payload scope, overlays, preview-bound authorization, atomicity, reports, and change publication.

The exact version 2 document shape is defined by the [library YAML format reference](../../../reference/library/format/yaml.md).
Library ownership and the storage/change pipeline are defined by [library architecture](../../../architecture/library.md).
CLI flags and output rendering belong to the [CLI command reference](../../../reference/cli/command.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
`LibraryYamlExporter`, `LibraryYamlImporter`, and `LibraryTaskService` translate between a portable document and `ao::library::MusicLibrary`; YAML is not a physical storage format.
Interactive and CLI callers use `LibraryTaskService` so a commit consumes preview evidence rather than accepting a bare path.

## Terminology

- **Source library** is the library being exported.
- **Target library** is the library being imported into.
- **Payload mode** is the document's `export_mode` and controls payload scope and baseline reconstruction.
- **Import mode** is `restore` or `merge` and controls how the payload combines with target state.
- **File baseline** is a track builder loaded from the audio file named by a Library URI when that file is readable.
- **Merge baseline** is an existing target track matched by canonical manifest URI.
- **Import plan** is a move-only, one-shot authorization containing the prepared payload, preview report, exact source bytes, target library identity and revision, and runtime identity.
- **Present collection** means `tags`, `custom`, or `covers` exists in a track record, including an explicitly empty sequence or map.

## Invariants

- One export observes metadata, tracks, lists, resources, dictionary values, and manifest facts through one read transaction.
- Version 2 uses the closed schema and explicit collection scope defined by the format reference.
- Every URI crossing YAML, manifest, Writer, or scan boundaries becomes a `LibraryUri`; playback, read-model, fingerprint, export/import baseline, and scan-apply access resolve it again beneath the weakly canonical root and reject escaping or unresolved symlinks. An absent root or ordinary missing suffix remains valid for first-run metadata restore.
- Import validates the complete document before applying any persistent mutation.
- One committed import applies content and any adopted `libraryId` through one write transaction and one library revision.
- Preview runs the same mutation path in an uncommitted transaction and publishes no content change.
- A prepared plan can commit only against the exact source bytes and target runtime, library identity, and committed revision it previewed.
- A collection field that is present replaces its complete baseline collection; an omitted collection preserves its baseline.
- Restore scope is determined by payload mode, never inferred from omitted collections.
- A payload has at most one track record for each canonical URI; merge matches tracks only by canonical manifest URI, and payload track IDs exist only for intra-payload references.
- Lists in the payload are recreated with new target IDs and then have parents remapped.

## State model

The application import path has two operations:

```text
prepare(path, import mode)
  -> read exact source bytes
  -> parse and validate version 2
  -> prepare track/list data
  -> capture target runtime + library id + committed revision
  -> run mutation path in an uncommitted preview transaction
  -> return LibraryImportPlan + ImportReport

explicit authorization
  -> consume LibraryImportPlan
  -> recheck runtime + library id + committed revision
  -> reread and compare exact source bytes
  -> run prepared mutation path in one write transaction
  -> commit once
  -> publish one change set
```

Rejecting or dropping a plan performs no persistent mutation.
A plan has no time-based expiry; its source and target bindings make stale plans unusable.

The synchronous offline importer remains a lower-level composition and test surface.
It provides preview and atomic import behavior but is not the frontend authorization boundary.

## Commands and transitions

### Export modes

| Payload mode | Metadata | Custom metadata | Tags | Covers | Technical and manifest facts | Lists |
|---|---|---|---|---|---|---|
| `delta` | Fields different from a readable file baseline; otherwise all non-empty fields. | Complete map when non-empty. | Complete sequence when non-empty. | Complete sequence when different from a readable file baseline; otherwise omitted. | Omitted. | Included. |
| `metadata` | All non-empty curated metadata. | Complete map when non-empty. | Complete sequence when non-empty. | Always present, including empty. | Omitted. | Included. |
| `full` | All non-empty curated metadata. | Complete map when non-empty. | Complete sequence when non-empty. | Always present, including empty. | Included, including zero values. | Included. |
| `listOnly` | No track records. | No track records. | No track records. | No track records. | No track records. | Included with URI membership references. |

The exporter constructs the complete YAML tree before opening the destination file.
For `delta`, a missing, unsupported, or unreadable audio file means no baseline and causes emission of all applicable current values.
A filesystem inspection error fails export with `IoError`.

### Restore

For `delta`, `metadata`, and `full`, restore clears tracks, manifest rows, and lists inside the import transaction before rebuilding them.
For `listOnly`, restore preserves tracks and manifest rows and clears only lists.

Restore chooses a track baseline by payload mode:

- `full` starts from an empty track and applies payload values;
- `delta` starts from a readable file baseline when available;
- `metadata` may retain file technical properties and cover resources, but clears file-derived curated metadata, tags, and custom metadata before applying the payload;
- when an optional file baseline cannot be opened or parsed, restore starts from an empty track.

If a track-bearing payload contains `libraryId`, restore writes it in the same transaction as restored content.
An absent `libraryId` preserves target identity.
`listOnly` restore always preserves target identity because its target scope is lists rather than the whole library.
Merge never adopts `libraryId`.

### Merge

Merge preserves target tracks and lists absent from the payload.
An imported track whose canonical URI matches a target manifest row updates that track; an unmatched URI creates a track and manifest row.

The existing target track is the merge baseline.
For `delta` and `metadata`, a readable source file refreshes technical properties; delta also supplies file cover art when the baseline has none.
Payload fields then overlay that baseline.

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
Manifest facts start from an existing manifest row, otherwise current filesystem facts when the path exists, otherwise zero.
Present `fileSize` and `mtime` fields override those facts.

### Lists

Lists are created in payload order, then parent relationships are applied in a second pass so a child may precede its parent.
Manual membership resolves payload IDs through tracks created or updated by this import and URI references through the target manifest.

A Smart List filter must be non-empty and must parse and compile under the current query grammar.
Known payload parent relationships must be self-free and acyclic before any list is written.
Every recreated list must fit the fixed-width core list layout; an oversized text field, track array, or combined offset rejects the import instead of truncating data.

Unresolved parent and track references are ignored and counted.
Manual membership keeps the first resolved occurrence and preserves first-occurrence order.

### Reports

Every import, preview, and plan returns an `ImportReport`:

| Field | Meaning |
|---|---|
| `payloadVersion` | Accepted interchange version; currently `2`. |
| `payloadMode` | `delta`, `metadata`, `full`, or `listOnly`. |
| `targetScope` | `Library` for track-bearing payloads or `Lists` for `listOnly`. |
| `tracksCreated` | Imported records that do not match a merge baseline. |
| `tracksUpdated` | Imported records that match a target manifest URI. |
| `tracksDeleted` | Pre-restore tracks when scope clears tracks; otherwise `0`. |
| `listsCreated` | Payload lists created by the import. |
| `listsDeleted` | Pre-restore lists; otherwise `0`. |
| `danglingReferencesIgnored` | Unresolved parent, payload-track-ID, and manifest-URI references. |

Counts describe processed matches and creations, not only byte-different records.
Preview and commit produce the same report when source bytes and target binding remain unchanged.

### Change publication

A committed application import publishes one `LibraryChangeSet` carrying the transaction revision.
Preview publishes nothing.

Restore publishes `libraryReset: true` and no incremental ID lists.
Merge publishes `libraryReset: false` with newly inserted tracks, matched tracks as mutated, and newly created lists as upserted.

## Failure and cancellation

File-read failures report `IoError`.
Malformed YAML, unsupported versions, closed-schema violations, invalid values, and unsafe URIs report `FormatRejected` as defined by the [format validation rules](../../../reference/library/format/yaml.md#validation-rules).
Applying a plan to different source bytes, another runtime, another library identity, or another target revision reports `Conflict`.
Every apply attempt consumes the plan, including an attempt that returns a pre-commit error; replay reports `InvalidState` and a retry requires a fresh preview.

Any failure before commit leaves target content, metadata identity, and revision unchanged and publishes no content change.
Commit failure likewise publishes no change set.

After durable commit, publication enqueue or observer failure follows [library change publication](change-publication.md#failure-and-lifetime): durable state is not rolled back or reported as a retryable import failure, and the live runtime enters terminal `Faulted`.

`LibraryTaskService` honors cancellation on executor transitions.
Once synchronous transfer work begins it has no internal stop checkpoint; after a possible commit it returns to the callback executor without reinterpreting committed state as cancelled.
The operation matrix belongs to [library task execution](task-execution.md#cancellation).

Version 2 currently defines no transfer-specific total-document, aggregate-cover, or per-cover byte budget beyond the exact field and core-storage limits in the format reference.
No configurable prepared-memory ceiling, streaming path, or additional bounded-transfer proposal is currently defined.
Adding a limit must preserve the guarantee that the current exporter cannot produce a file the importer rejects solely for size.

## Persistence and versioning

Version 2 is a portable interchange format, not the physical database format.
Restore and merge always write current `MusicLibrary` records.
The importer accepts no earlier interchange version and provides no migration or legacy-restore path.

## Frontend observations

CLI and GTK use the same plan-producing runtime operation and one-shot apply operation.
Neither frontend may commit a restore from a bare path or reinterpret scope, counts, matching, or publication.

GTK prepares a restore plan after file selection, presents its version, payload mode, scope, counts, and ignored references, and applies only after an explicit positive response.
Closing or rejecting the confirmation drops the plan.

CLI defaults import to `merge`.
`--mode restore --dry-run` prepares and prints a plan report without committing; a non-dry-run restore additionally requires `--confirm-destructive-restore`.
The apply step still revalidates source and target evidence, so the flag cannot authorize a changed preview.

## Implementation map

- [`LibraryYamlExporter`](../../../../app/include/ao/rt/library/LibraryYamlExporter.h) and [`LibraryYamlExporter.cpp`](../../../../app/runtime/library/LibraryYamlExporter.cpp) implement export modes and baselines.
- [`LibraryYamlImporter`](../../../../app/include/ao/rt/library/LibraryYamlImporter.h) and [`LibraryYamlImporter.cpp`](../../../../app/runtime/library/LibraryYamlImporter.cpp) implement strict parsing and prepared mutation behavior.
- [`LibraryImportPlan`](../../../../app/include/ao/rt/library/LibraryImportPlan.h) and [`LibraryTaskService`](../../../../app/include/ao/rt/library/LibraryTaskService.h) define preview-bound application authorization.
- [`LibraryUri`](../../../../include/ao/library/LibraryUri.h) defines canonical root-relative path evidence.
- [`LibraryChanges`](../../../../app/include/ao/rt/library/LibraryChanges.h) defines published change values.

## Test map

- [`LibraryExportImportTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportTest.cpp) proves mode baselines, overlays, reports, and preview equivalence.
- [`LibraryExportImportDeltaTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportDeltaTest.cpp) proves delta behavior, identity adoption, rollback, and change sets.
- [`LibraryExportImportCoverArtTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportCoverArtTest.cpp) proves cover round trips and replacement.
- [`LibraryExportImportListTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportListTest.cpp) proves list-only transfer, references, remapping, and dangling counts.
- [`LibraryYamlSchemaTest.cpp`](../../../../test/unit/runtime/library/LibraryYamlSchemaTest.cpp) proves closed-schema, scope, enum, URI, duplicate-key, list-semantic, and storage-limit rejection.
- [`LibraryExportImportErrorTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportErrorTest.cpp) proves scalar rejection and transactional rollback.
- [`LibraryTaskServiceTest.cpp`](../../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) proves source/target binding, one-shot plans, cancellation before maintenance, and mandatory callback completion after commit.
- [`LibraryImportExportWorkflowTest.cpp`](../../../../test/unit/linux-gtk/portal/LibraryImportExportWorkflowTest.cpp) proves confirmation precedes GTK mutation.
- [`CliSmokeTest.cpp`](../../../../test/unit/cli/CliSmokeTest.cpp) proves CLI preview and explicit restore confirmation.

## Related documents

- [Library YAML format reference](../../../reference/library/format/yaml.md)
- [Library architecture](../../../architecture/library.md)
- [Outcome channel specification](../../failure/outcome-channel.md)
- [Error value reference](../../../reference/failure/error.md)
