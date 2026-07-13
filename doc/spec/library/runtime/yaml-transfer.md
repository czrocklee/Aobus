---
id: library.yaml-transfer
type: spec
status: current
domain: library
summary: Defines export, restore, merge, preview, reporting, and change-publication behavior for library YAML transfers.
---
# Library YAML transfer

## Scope

This specification defines the behavior of library YAML export and import.
It owns mode semantics, baselines, payload scope, overlay rules, atomicity, reports, previews, and change publication.

The exact version 1 document shape is defined by the [library YAML format reference](../../../reference/library/format/yaml.md).
Library ownership and the storage/change pipeline are defined by [library architecture](../../../architecture/library.md).
CLI flags, command spelling, and output rendering belong to the [CLI command reference](../../../reference/cli/command.md).

## Code boundary

This contract belongs to the **application runtime** layer in the [system architecture](../../../architecture/system-overview.md).
`LibraryYamlExporter`, `LibraryYamlImporter`, and their asynchronous adaptation live under `ao::rt::library`; they translate between a portable document and the core `ao::library::MusicLibrary` without making YAML a physical storage format.

## Terminology

- **Source library** is the `MusicLibrary` being exported.
- **Target library** is the `MusicLibrary` being imported into.
- **Payload mode** is the `export_mode` recorded in the YAML document and controls payload scope and baseline reconstruction.
- **Import mode** is `restore` or `merge` and controls how the payload combines with target state.
- **File baseline** is a `TrackBuilder` loaded from the audio file at the track URI when that file can be opened and parsed.
- **Merge baseline** is an existing target track matched by normalized manifest URI.
- **Present collection** means `tags`, `custom`, or `covers` exists in a track record, including an explicitly empty sequence or map.

## Invariants

- One export observes metadata header, tracks, lists, resources, dictionary values, and manifest facts through one library read transaction.
- Import parses and structurally validates the complete document before opening its write transaction.
- One committed import applies content and an adopted `libraryId` through one write transaction and one library revision.
- Preview executes the same validation and mutation path but commits no storage, metadata header, or change event.
- A collection field that is present replaces its complete baseline collection; an omitted collection preserves its baseline.
- Restore scope is determined by payload mode, not by which optional sequences happen to be present.
- Merge matches tracks only by normalized manifest URI; payload track IDs are for intra-payload list references.
- Lists in the payload are recreated with new target IDs and then have parents remapped.

## State model

An import has these phases:

```text
read and parse
  -> validate complete payload
  -> collect pre-import report/change facts
  -> open write transaction
  -> clear restore scope when requested
  -> import tracks and manifest rows
  -> create lists and remap parents
  -> preview: abort by leaving transaction uncommitted
  -> commit: commit once, then publish one changeset when configured
```

No target mutation is visible before the commit phase.

## Commands and transitions

### Export modes

| Payload mode | Metadata | Custom metadata | Tags | Covers | Technical and manifest facts | Lists |
|---|---|---|---|---|---|---|
| `delta` | Fields different from the readable file baseline; otherwise all non-empty fields. | Complete map when non-empty. | Complete sequence when non-empty. | Complete sequence when different from the readable file baseline; otherwise omitted. | Omitted. | Included. |
| `metadata` | All non-empty curated metadata. | Complete map when non-empty. | Complete sequence when non-empty. | Always present, including empty. | Omitted. | Included. |
| `full` | All non-empty curated metadata. | Complete map when non-empty. | Complete sequence when non-empty. | Always present, including empty. | Included, including zero values. | Included. |
| `listOnly` | No track records. | No track records. | No track records. | No track records. | No track records. | Included with URI membership references. |

The exporter keeps one read transaction for the complete document.
It writes the file only after the in-memory YAML tree has been constructed successfully.

For `delta`, each source URI is inspected for a file baseline.
A missing file, unsupported file, tag-open failure, or tag-load failure means no baseline and causes the exporter to emit all non-empty metadata plus the current non-empty custom metadata and tags.
A filesystem error while inspecting whether the path exists fails export with `IoError`.

### Restore

For a payload other than `listOnly`, restore clears tracks, manifest rows, and lists inside the import transaction before rebuilding from the payload.
For `listOnly`, restore preserves tracks and manifest rows and clears only lists.

Restore chooses a track baseline by payload mode:

- `full` starts from an empty track and applies only payload values.
- `delta` starts from a readable file baseline when available, then overlays the payload.
- `metadata` may use a readable file for technical properties and cover resources, but clears file-derived curated metadata, tags, and custom metadata before applying the payload.
- When the required file baseline cannot be opened or parsed, restore starts from an empty track without failing.

If the payload contains `libraryId`, restore writes it in the same transaction as the restored content.
An absent `libraryId` preserves the target library identity.
Merge never adopts `libraryId`.

### Merge

Merge preserves tracks and lists outside the payload.
Each imported track whose normalized URI matches a target manifest row updates that existing target track; an unmatched URI creates a new track and manifest row.

The existing target track is the merge baseline.
For `delta` and `metadata`, a readable source file refreshes the baseline's technical properties; delta also supplies file cover art when the existing baseline has none.
Payload fields then overlay that baseline.

Merge does not match or update existing lists.
Every payload list is created as a new target list, after which payload parent IDs are remapped to the newly allocated target IDs.

### Track overlays

Present metadata and technical scalar fields replace the corresponding baseline value.
Omitted scalar fields preserve the baseline value.

`tags`, `custom`, and `covers` use collection replacement semantics:

- omitted field: preserve the baseline collection;
- present non-empty field: replace the complete baseline collection;
- present empty sequence or map: clear the complete baseline collection.

An accepted codec name replaces the baseline codec.
An unrecognized codec name leaves the baseline unchanged.

Manifest facts start from an existing manifest row when present, otherwise from current filesystem size and modification time when the source path exists, otherwise from zero.
Present `fileSize` and `mtime` payload fields override those baseline facts.

### Lists

Lists are created in payload order, then parent relationships are applied in a second pass so a child may appear before its parent.
Manual membership resolves payload track IDs through tracks created or updated by this import and resolves URI references through the target manifest.

Unresolved parent and track references are ignored.
Manual membership keeps the first resolved occurrence of a track and preserves first-occurrence order.

### Reports

Every import and preview returns an `ImportReport`:

| Field | Meaning |
|---|---|
| `tracksCreated` | Imported track records that did not match a merge baseline. |
| `tracksUpdated` | Imported track records that matched an existing target manifest URI. |
| `tracksDeleted` | Tracks in the pre-restore target when payload scope clears tracks; otherwise `0`. |
| `listsCreated` | Payload list records created by the import. |
| `listsDeleted` | Lists in the pre-restore target; otherwise `0`. |

The report counts processed matches and creations, not only records whose serialized bytes differ.
Preview returns the same report that the corresponding commit path would return against the same starting state.

### Change publication

When the importer has a `LibraryChanges` collaborator, a committed import publishes one `LibraryChangeSet` carrying the transaction's library revision.
Preview publishes nothing.

Restore publishes `libraryReset: true` and no incremental ID lists.
The reset covers content and any adopted `libraryId` because both commit under the same revision.

Merge publishes `libraryReset: false` with:

- `tracksInserted` for target tracks newly created by this import;
- `tracksMutated` for imported URIs that matched tracks present before the import;
- `listsUpserted` for lists newly created by this import.

Merge import does not populate track/list deletion or manual-list operation fields.

## Failure and cancellation

File-read failures before parsing report `IoError`.
Malformed YAML, unsupported versions, invalid node kinds or scalar widths, invalid UUIDs, and invalid cover data report `FormatRejected` as defined by the [format validation rules](../../../reference/library/format/yaml.md#validation-rules).

Validation finishes before the write transaction begins.
Any failure while clearing, building, serializing, or writing a record leaves that transaction uncommitted, so target content and metadata header remain unchanged.
Commit failure likewise publishes no changeset.

The synchronous exporter/importer APIs have no stop-token surface.
`LibraryTaskService` owns asynchronous scheduling, but once synchronous transfer work begins it has no in-operation cancellation checkpoint; the exact boundary is defined by [library task execution](task-execution.md#cancellation).

## Persistence and versioning

Version 1 payloads are portable interchange documents, not the physical library database.
Restore and merge always write through the current `MusicLibrary` stores and therefore produce the current physical storage version.

Payload versioning and compatibility are owned by the [format reference](../../../reference/library/format/yaml.md#compatibility-and-versioning).
An export never emits the legacy `minimum` mode token even though import accepts it.

## Frontend observations

CLI and GTK use the same runtime exporter/importer behavior.
CLI owns argument parsing, dry-run flag spelling, report rendering, and exit-code mapping.
GTK owns chooser/dialog lifecycle and maps task progress and completion to frontend presentation.

Neither frontend may reinterpret restore scope, merge matching, preview, report counts, or change publication.

## Implementation map

- [`LibraryYamlExporter`](../../../../app/include/ao/rt/library/LibraryYamlExporter.h) and [`LibraryYamlExporter.cpp`](../../../../app/runtime/library/LibraryYamlExporter.cpp) implement export modes and baselines.
- [`LibraryYamlImporter`](../../../../app/include/ao/rt/library/LibraryYamlImporter.h) and [`LibraryYamlImporter.cpp`](../../../../app/runtime/library/LibraryYamlImporter.cpp) implement validation, restore, merge, preview, reports, and publication.
- [`LibraryTaskService`](../../../../app/include/ao/rt/library/LibraryTaskService.h) provides the asynchronous application boundary.
- [`LibraryChanges`](../../../../app/include/ao/rt/library/LibraryChanges.h) defines published library change values.
- [`MusicLibrary`](../../../../include/ao/library/MusicLibrary.h) and [`MetadataStore`](../../../../include/ao/library/MetadataStore.h) provide the single-transaction content/header commit boundary.

## Test map

- [`LibraryExportImportTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportTest.cpp) proves full restore, merge overlays, collection omission/clearing, URI normalization, reports, and preview equivalence.
- [`LibraryExportImportDeltaTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportDeltaTest.cpp) proves delta baselines, inspection failures, atomic library-ID adoption, preview non-mutation, and changesets.
- [`LibraryExportImportCoverArtTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportCoverArtTest.cpp) proves ordered cover round trips and replace/clear behavior.
- [`LibraryExportImportListTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportListTest.cpp) proves list-only transfer, ID/URI references, parent remapping, dangling references, and membership order.
- [`LibraryExportImportErrorTest.cpp`](../../../../test/unit/runtime/library/LibraryExportImportErrorTest.cpp) proves structural and scalar rejection.
- [`LibraryTaskServiceTest.cpp`](../../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) proves the async worker/callback integration.
- [`CliSmokeTest.cpp`](../../../../test/unit/cli/CliSmokeTest.cpp) proves CLI export/import and dry-run adaptation.

## Related documents

- [Library YAML format reference](../../../reference/library/format/yaml.md)
- [Library architecture](../../../architecture/library.md)
- [Outcome channel specification](../../failure/outcome-channel.md)
- [Error value reference](../../../reference/failure/error.md)
- [Track model](../../../reference/library/model/track.md)
