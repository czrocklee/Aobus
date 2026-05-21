# Smart Restoration and List Management Implementation Plan

## Purpose

This plan defines the one-shot implementation for Aobus smart library
export/import. The implementation may be built internally through multiple
phases, but the user-facing result must land as one coherent feature set: real
merge import, true list-only synchronization, complete full backups,
metadata-preserving imports, delta synchronization, and cover art restoration.

The existing `minimum`, `metadata`, and `full` export modes are not enough as a
complete contract. This plan replaces the ambiguous minimum mode with explicit
delta semantics and adds list-only exports for playlist synchronization.

## Goals

- Make `full` export/import a deterministic disaster-recovery path that does
  not require physical audio files during restore.
- Make `metadata` export/import preserve curated text metadata and cover art
  while allowing technical properties to be refreshed from files.
- Make `delta` export/import synchronize user changes by comparing database
  state against physical file tags.
- Make `listOnly` export/import synchronize lists without mutating track data.
- Make `merge` import truly update existing tracks by URI instead of creating
  duplicates.
- Preserve manual list membership across libraries by using track URIs rather
  than internal `TrackId` values where needed.
- Round-trip cover art through Base64 resource payloads, using
  `ResourceStore::Writer::create()` for content-addressed deduplication.
- Keep import validation front-loaded so destructive restore does not clear data
  before the YAML payload is known to be valid.

## Non-Goals

- Writing metadata back to audio files.
- Reorganizing, renaming, or moving audio files.
- Multi-root library aggregation.
- Automatic conflict resolution UI for simultaneous edits. Initial merge
  conflict behavior is deterministic and field-based.
- Guaranteeing that `delta` is a complete backup. Delta is a synchronization
  format and depends on physical audio files for omitted baseline fields.

## Terminology

### Payload Mode

The `export_mode` field in YAML describes what the payload contains:

```text
delta
metadata
full
listOnly
```

Payload mode is read from the file and is independent of the import strategy.

### Import Strategy

The import command chooses how the payload mutates the current library:

```text
restore
  Destructive within the payload scope.

merge
  Additive/update import. Preserve existing records outside the payload scope.
```

## User-Facing Behavior Matrix

```text
Payload contains tracks + restore
  Clear tracks, lists, and manifests after validation. Rebuild them from the
  payload and, where required by payload mode, physical file baselines.

Payload contains tracks + merge
  Match by URI. Update existing tracks. Create missing tracks. Preserve tracks
  and lists not mentioned by the payload, except lists explicitly imported.

listOnly + restore
  Clear lists only after validation. Rebuild lists by matching list track URIs
  against existing tracks. Do not mutate tracks or manifests.

listOnly + merge
  Upsert lists from the YAML. Preserve existing lists not mentioned by the
  payload. Do not mutate tracks or manifests.
```

## Export Mode Contracts

### `ExportMode::Delta`

Intent: smallest useful synchronization file for user edits.

Export rules:

- Always include track `id` and `uri` for track payloads.
- Read the physical audio file through `tag::TagFile::open()` when available.
- Build a baseline `TrackBuilder` or equivalent view from the file tags.
- Compare baseline file metadata against the library track view.
- Emit only fields whose database values differ from the file baseline.
- Always emit Aobus-only user data when present:
  - rating
  - Aobus tags
  - custom metadata
- Emit cover art only when the database cover resource differs from the file
  baseline cover art.
- Do not emit technical properties unless they are Aobus-only fields in a future
  schema. Current technical properties are baseline data and are omitted.

Missing physical file behavior:

- Delta export must not silently produce a misleading complete track record.
- If a physical file is missing, export a sparse track with `uri` and Aobus-only
  fields, plus a per-track `baselineMissing: true` marker.
- The importer must treat `baselineMissing: true` as a warning condition for
  restore because omitted metadata cannot be reconstructed.

### `ExportMode::Metadata`

Intent: preserve curated metadata and cover art without bulky technical state.

Export rules:

- Emit `id` and `uri`.
- Emit all curated text metadata, when present:
  - title
  - artist
  - album
  - albumArtist
  - composer
  - genre
  - work
  - year
  - trackNumber
  - totalTracks
  - discNumber
  - totalDiscs
- Emit all Aobus-only user data:
  - rating
  - Aobus tags
  - custom metadata
- Emit cover art as a Base64 resource payload when present.
- Do not emit technical properties:
  - durationMs
  - bitrate
  - sampleRate
  - channels
  - bitDepth
  - codecId
  - fileSize
  - mtime

### `ExportMode::Full`

Intent: deterministic disaster recovery.

Export rules:

- Emit everything from `metadata`.
- Emit all available technical properties:
  - durationMs
  - bitrate
  - sampleRate
  - channels
  - bitDepth
  - codecId
  - fileSize
  - mtime
- Emit FileManifest-derived state when available.
- Emit cover art resource payloads.
- Importing a full payload in restore mode must not read physical files.

### `ExportMode::ListOnly`

Intent: synchronize playlists without touching track records.

Export rules:

- Omit the `tracks` node entirely.
- Emit `lists`.
- For manual lists, emit track membership by relative `uri`, not internal
  `TrackId`.
- For smart lists, emit the filter expression unchanged.
- Include list IDs and parent IDs only as YAML-local IDs for reconstructing list
  hierarchy. They are never reused as internal database IDs.

## YAML Shape

The root remains versioned:

```yaml
version: 1
libraryId: 01234567-89ab-cdef-0123-456789abcdef
export_mode: full
library:
  tracks:
    - id: 1
      uri: album/song.flac
      title: Example
      artist: Example Artist
      coverArtBase64: "..."
      durationMs: 180000
  lists:
    - id: 1
      parentId: 0
      name: Favorites
      tracks:
        - id: 1
```

For `listOnly`, manual list membership uses URI records:

```yaml
version: 1
export_mode: listOnly
library:
  lists:
    - id: 1
      parentId: 0
      name: Favorites
      tracks:
        - uri: album/song.flac
```

For non-list-only payloads, manual lists may continue to use YAML-local track
IDs. The importer should also accept URI list entries so that the format can be
made uniformly URI-based later without another breaking change.

## Cover Art Resource Encoding

### Field Name

Use a single field on each track:

```yaml
coverArtBase64: "..."
```

The decoded bytes are inserted with `ResourceStore::Writer::create()`. The
returned `ResourceId` is assigned to the track metadata cover art field. This
keeps importer-side deduplication simple and relies on the existing CAS store.

### YAML Anchors

After basic Base64 cover art round-trip is correct, the exporter should reduce
file size with YAML anchors:

```yaml
coverArtBase64: &cover_1 "..."
```

Subsequent tracks may refer to the same scalar:

```yaml
coverArtBase64: *cover_1
```

The importer must not need explicit anchor handling if yaml-cpp resolves aliases
as ordinary scalar nodes. Tests must include at least one alias-backed cover art
node to verify this behavior.

Anchor generation rules:

- Maintain an exporter-local map of `ResourceId` to anchor name.
- Anchor names are deterministic within one export, for example
  `cover_<resourceId>` or `cover_<ordinal>`.
- Do not expose database `ResourceId` as a semantic import dependency. Anchor
  names are YAML-local only.

## Smart Restoration Semantics

### Baseline Selection

The importer reads `export_mode` before opening a write transaction.

```text
full
  Do not read physical files. Build tracks entirely from YAML.

metadata
  Read physical file when present to recover technical properties. Apply YAML
  curated metadata and cover art.

delta
  Read physical file when present to recover baseline metadata, cover art, and
  technical properties. Apply sparse YAML changes.

listOnly
  Do not read physical files. Match list item URIs against existing tracks.
```

### Missing Physical Files

Importer behavior must be explicit:

- `full`: missing files do not matter.
- `metadata`: missing files are allowed; imported tracks may have zero/default
  technical properties unless the destination already has an existing track in
  merge mode.
- `delta`: missing files mean omitted baseline fields cannot be reconstructed.
  Restore mode should report a clear error unless the track is marked
  `baselineMissing: true`, in which case the importer may create a sparse track
  and report a warning. Merge mode may apply sparse fields to an existing URI
  without reading the file.
- `listOnly`: missing track URIs in lists are skipped or reported according to a
  deterministic policy. The initial policy should be strict in tests for invalid
  YAML but non-destructive for user data: do not create placeholder tracks.

## Real Merge Import

Merge import must be completed as part of this feature. The current append-only
behavior is not acceptable.

### URI Matching

Before importing tracks in merge mode:

- Build a map of existing `uri -> TrackId` from TrackStore/FileManifest.
- For every YAML track:
  - if URI exists, update that track in place;
  - if URI does not exist, create a new track;
  - record `yamlTrackId -> internal TrackId` for list remapping.

### Update Rules

For an existing track:

- Start from the existing database record.
- If payload mode requires a physical baseline and the file exists, refresh the
  baseline fields needed by that mode.
- Apply YAML fields over the selected baseline.
- Preserve fields omitted by the payload unless the payload mode defines them as
  absent-by-contract.
- Update FileManifest fileSize/mtime when a physical baseline is read or when a
  full payload supplies explicit values.

For a missing track created by merge:

- `full`: create entirely from YAML.
- `metadata`: create from physical file if present, then apply YAML; otherwise
  create with URI and curated metadata.
- `delta`: create from physical file if present, then apply YAML; otherwise
  fail or create a sparse warning record only for `baselineMissing: true`.

### List Merge

List merge must upsert by stable matching rules. Initial matching rule:

- Match by full list path `(parent path + name)` after reconstructing YAML-local
  hierarchy.
- If a matching list exists, replace its imported fields:
  - name
  - description
  - filter for smart lists
  - manual track membership
- If no matching list exists, create it.
- Preserve existing lists whose path is not present in the YAML.

Restore mode continues to replace all lists within payload scope.

## Validation Before Mutation

Import must validate the payload before destructive changes:

- supported `version`
- supported `export_mode`
- required `library` section
- `tracks` present for track payloads and absent/ignored for `listOnly`
- every track has a non-empty `uri`
- unique YAML track IDs when IDs are present
- unique YAML list IDs
- valid list parent references
- manual list track references are valid for their payload form:
  - YAML-local track IDs for track payloads
  - URIs for `listOnly`
  - either form when accepting compatibility input
- numeric field ranges fit target types
- Base64 cover art decodes successfully
- `delta` restore has required physical baseline or explicit sparse fallback
  markers

Only after validation should the importer open a write transaction and clear or
mutate stores.

## Implementation Phases

These phases are implementation checkpoints, not separate product releases. The
final merged behavior should satisfy all contracts above.

### Phase 1: Schema and Mode Plumbing

- Rename or replace `ExportMode::Minimum` with `ExportMode::Delta`.
- Add `ExportMode::ListOnly`.
- Update CLI mode parsing and GTK export mode selection.
- Add parser helpers for payload mode strings.
- Keep backward compatibility for existing `minimum` YAML by treating it as
  legacy `delta` with no physical-baseline guarantees.
- Add import context object containing:
  - payload mode
  - import strategy
  - validation results
  - warning collection

### Phase 2: Front-Loaded Import Validation

- Parse YAML into lightweight validated import records before mutation.
- Validate track records, list hierarchy, list membership, and cover art scalar
  shape.
- Move destructive `clear()` calls after validation.
- Implement payload-scope clearing:
  - track payload restore clears tracks, lists, manifests, and resources only as
    needed by restored tracks;
  - `listOnly` restore clears lists only.

### Phase 3: ListOnly URI Export/Import

- Export `listOnly` without `tracks`.
- Emit manual list track membership as URI entries.
- Build destination URI lookup for importing list membership.
- Restore mode clears and rebuilds lists only.
- Merge mode upserts matching lists and preserves unrelated lists.

### Phase 4: Real Merge Track Import

- Build URI-to-track map from current library state.
- Update existing tracks by URI instead of creating duplicates.
- Create missing tracks according to payload mode rules.
- Preserve omitted fields correctly for sparse payloads.
- Ensure list remapping works for both updated and newly created tracks.

### Phase 5: Base64 Cover Art Round-Trip

- Add Base64 encode/decode utility in an existing suitable utility location, or
  a small runtime-local helper if no shared utility exists.
- Export cover art payloads in `metadata` and `full`.
- Export cover art payloads in `delta` only when database art differs from file
  baseline.
- Decode imported cover art and call `ResourceStore::Writer::create()`.
- Assign the resulting resource ID to track metadata.
- Add YAML anchor emission and alias round-trip tests after the plain scalar path
  is correct.

### Phase 6: Full and Metadata Smart Restore

- Make `full` restore bypass physical file reads.
- Make `metadata` restore read physical files only for technical baseline data.
- If physical files are absent for metadata restore, keep technical fields at
  defaults for newly created tracks and preserve existing technical fields in
  merge mode.
- Ensure FileManifest values are restored from full payloads and refreshed from
  physical files for metadata payloads.

### Phase 7: Delta Diff Export and Sparse Import

- Implement file-baseline loading for delta export.
- Compare database metadata, tags, custom fields, rating, and cover art against
  file baseline.
- Emit only changed/extra fields.
- Implement delta restore/merge baseline reconstruction.
- Add explicit missing-baseline diagnostics.

### Phase 8: CLI, GTK, and Documentation Polish

- Update CLI help text to describe modes accurately:
  - `delta` for syncing user edits
  - `metadata` for curated metadata backup without technical state
  - `full` for disaster recovery
  - `listOnly` for playlists
- Update GTK export mode labels and descriptions.
- Update `03-backup-restore.md` after implementation to reflect final behavior
  rather than intended behavior.

## Test Plan

All changes require focused tests because export/import is a backup path.

### Unit Tests

- Mode string parser accepts `delta`, `metadata`, `full`, `listOnly`, and legacy
  `minimum` where compatibility is intended.
- Base64 utility round-trips empty, small, binary, and non-multiple-of-three
  inputs.
- Import validation rejects duplicate track/list IDs, bad parents, bad numeric
  ranges, invalid Base64, and missing required URIs.

### Export/Import Round-Trip Tests

- Full export/import restores all text metadata, rating, tags, custom metadata,
  technical properties, manifest state, and cover art without physical files.
- Metadata export/import restores curated metadata and cover art while omitting
  technical properties from YAML.
- Delta export emits only fields changed from physical file baseline.
- Delta import applies sparse fields on physical file baseline.
- ListOnly restore clears only lists and does not alter existing tracks.
- ListOnly merge upserts imported lists and preserves unrelated lists.

### Merge Tests

- Merging a track payload with an existing URI updates that track and does
  not create a duplicate.
- Merging a payload with a new URI creates a new track.
- Merge preserves tracks not mentioned in the YAML.
- Merge remaps manual list membership to both updated and newly created
  tracks.
- Merge preserves existing list records not present in the YAML.

### Cover Art Tests

- Full export/import round-trips cover art bytes.
- Metadata export/import round-trips cover art bytes.
- Identical cover art imported for multiple tracks deduplicates to one
  `ResourceId` through `ResourceStore::Writer::create()`.
- YAML alias-backed cover art nodes import correctly.
- Delta exports cover art only when database art differs from physical file art.

### Missing File Tests

- Full restore succeeds when all audio files are absent.
- Metadata restore succeeds with absent files and default technical properties.
- Delta restore reports a clear error or warning for absent baseline according
  to `baselineMissing` policy.
- ListOnly import with missing destination track URI does not create placeholder
  tracks.

## Verification Commands

Use the repository standard validation path from the project root:

```bash
./build.sh debug
```

For focused iteration, run the runtime export/import tests through the configured
debug test binary once the build exists. If C++ files are changed, run the
project clang-tidy wrapper on the changed files before finalizing.

## Migration and Compatibility

- Existing YAML with `export_mode: minimum` should remain importable as legacy
  delta. Export should use `delta` going forward.
- Existing full/metadata YAML without cover art remains valid; missing
  `coverArtBase64` means no imported cover override.
- Existing manual list track references by numeric YAML track ID remain valid for
  non-list-only payloads.
- Internal LMDB IDs from YAML are never reused directly. YAML IDs are local
  remapping keys only.

## Acceptance Criteria

- `full + restore` can rebuild a library, including cover art and technical
  properties, with no audio files present.
- `metadata + restore` can rebuild curated metadata and cover art, using files
  only for technical baseline data when available.
- `delta + merge` can apply user edits to a scanned library without replacing
  unchanged file-derived metadata.
- `listOnly + restore` modifies lists only.
- `listOnly + merge` preserves unrelated lists.
- Merge import by URI does not duplicate existing tracks.
- Manual lists can be moved across libraries through URI-based membership.
- Destructive restore does not clear data before validation succeeds.
