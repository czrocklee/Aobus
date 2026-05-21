# Library Management Implementation Plan

## Purpose

This plan turns the library management design into staged, reviewable work. It
preserves the root library model, adds external DB support, makes backup
trustworthy, and introduces FileManifest as the first-class physical file
tracking subsystem.

Design contracts:

- `doc/design/library-management/00-overview.md`
- `doc/design/library-management/01-library-profiles-and-storage.md`
- `doc/design/library-management/02-read-only-tag-metadata.md`
- `doc/design/library-management/03-backup-restore.md`
- `doc/design/library-management/04-file-manifest-scan-dedup.md`

## Success Criteria

- Existing portable libraries continue to open.
- New external profiles can store LMDB data outside the music root.
- Track URIs remain relative to one music root.
- Metadata edits continue to mutate only the database.
- Export/import round-trips all curated metadata.
- FileManifest is the source of truth for URI, fileSize, mtime, and
  availability.
- Re-importing the same file does not create duplicate tracks.
- Scanner can classify new, changed, missing, and unchanged files.
- Missing files are never deleted automatically.

## Phase 0: Document and Test Current Behavior

### Work

- Add tests that capture current read-only tag behavior where possible.
- Add export/import regression tests for existing fields before changing the
  format.
- Record current import duplicate behavior as a known gap.

### Verification

```bash
./build.sh debug
```

## Phase 1: Complete Logical Export/Import

### Intent

Make YAML export/import credible as the backup path for DB metadata.

### Changes

1. Export and import missing curated metadata:
   - composer
   - work

2. Restore full-mode file state:
   - fileSize
   - mtime
   - availability status, once FileManifest exists

3. Clarify `ExportMode::Full`:
   - either implement resource payload export/import
   - or update comments so full mode does not claim resource support yet

4. Add a restore path that does not read physical file tags.

### Tests

- Round-trip all metadata fields.
- Round-trip composer/work for classical tracks.
- Round-trip fileSize/mtime in full mode.
- Restore when physical files are absent.
- Ensure restore does not use file tag data to fill fields.

## Phase 2: Directory-Based Library Discovery

### Intent

Rely purely on directory structure and the global config "last library" bookmark for library discovery.

### Changes

1.  Eliminate `LibraryProfile` and `profile.yaml`.
2.  Update runtime to initialize with `musicRoot` and derive `databasePath`.
3.  Store logical session state in `workspace.yaml` within the database directory.

## Phase 3: Session State Logic

### Intent

Restore the user's working environment (open views, filters) consistently across restarts and frontends.

### Changes

1. Implement `WorkspaceSnapshot` for open views and active tab.
2. Route logic state to `workspace.yaml` in the database directory.
3. Route machine state (audio, last path) to global `config.yaml`.

## Phase 4: FileManifest Schema

### Intent

Make physical file tracking a first-class subsystem and remove URI/file-state
coupling from `TrackColdHeader`.

### Changes

1. Add `file_manifest` LMDB database:

   ```text
   key: normalized relative uri
   value: ManifestEntry { trackId, fileSize, mtime, status }
   ```

2. Update the LMDB database wrapper so FileManifest can be opened without
   `MDB_INTEGERKEY`. Existing integer-key stores may keep their current
   behavior, but URI-keyed manifest lookups require a non-integer key path.

3. Enforce a normalized URI key limit of 500 bytes. Import and scan should
   reject longer relative paths with a clear error before attempting to write
   the manifest entry.

4. Remove `fileSizeLo`, `fileSizeHi`, `mtimeLo`, and `mtimeHi` from
   `TrackColdHeader`.

5. Bump the library schema version.

6. Add a `FileManifestStore` core type with APIs for:
   - find by URI
   - find track by URI
   - create/update/delete entry
   - update file state
   - update availability status
   - iterate manifest entries

7. Update `MusicLibrary` initialization to create and expose the manifest store.

8. Update track property read paths so URI, fileSize, mtime, and availability
   come from FileManifest-aware APIs instead of `TrackColdHeader`.

9. Migrate development/legacy data by extracting URI, fileSize, and mtime from
   old cold records, writing manifest entries, and rewriting cold records using
   the slimmer layout.

### Tests

- Existing track URI lookup returns the correct track ID.
- Duplicate unchanged import is skipped.
- Changed existing import does not create a duplicate.
- Track deletion removes the FileManifest entry.
- Migration creates manifest entries and rewrites cold payloads.
- `TrackColdHeader` no longer stores fileSize or mtime.
- FileManifest rejects URI keys longer than 500 bytes.
- FileManifest uses string LMDB keys without `MDB_INTEGERKEY`.

## Phase 5: Conservative Refresh of Changed Files

### Intent

Handle changed files without destroying DB-curated metadata.

### Changes

1. When an existing URI has changed fileSize or mtime:
   - update FileManifest fileSize
   - update FileManifest mtime
   - refresh technical properties
   - preserve editable metadata

2. Add an explicit command for "Re-read tags from file" later. Do not combine it
   with ordinary import refresh.

### Tests

- Changed file updates file state.
- Changed file preserves title/artist/composer/work edited in DB.
- FileManifest and TrackStore updates occur in the same transaction.
- Explicit re-read tag command can be tested separately when added.

## Phase 6: Library Scanner

### Intent

Detect filesystem changes as a plan before applying them.

### Changes

1. Add a scanner service that walks the music root and compares filesystem
   snapshots with FileManifest entries.
2. Build a `ScanPlan` with:
   - new files
   - changed files
   - missing tracks
   - unchanged tracks
   - unsupported files
   - errors

3. Expose scan plan application commands:
   - import new
   - refresh changed
   - mark missing
   - remove missing
   - ignore

4. Keep GUI integration minimal at first. CLI or tests can drive the first
   implementation.

### Tests

- Scan classification for all states.
- Missing files are marked, not deleted.
- Unsupported files are reported.
- Scanner rejects files outside the music root.

## Phase 7: Missing State and UX

### Intent

Make offline NAS/external-disk behavior understandable and safe.

### Changes

1. Store availability state in FileManifest.
2. Surface missing status in track properties or row display.
3. Prevent playback failures from deleting tracks.
4. Add a user action to remove missing tracks from the library.

### Tests

- Missing track remains in library.
- Playback open failure can mark missing.
- Remove-missing action only removes selected DB records.

## Deferred Work

- Multi-source library aggregation.
- Multiple active databases in one UI.
- Automatic physical file organization.
- Automatic rename/move merging.
- Full-file hash deduplication during normal import.
- Acoustic fingerprint duplicate detection.
- File tag writing.

These are explicitly outside the first implementation because they increase
complexity without improving the core single-root library experience.
