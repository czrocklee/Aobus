# FileManifest, Scanning, and Duplicate Prevention

## Purpose

This document defines how Aobus tracks physical files, prevents duplicate
imports, and detects filesystem changes while preserving the single-root library
model.

The design intentionally separates curated track data from physical file state:

```text
TrackStore
  what the track is: metadata, tags, audio properties, custom data

FileManifest
  where the file lives: URI, size, mtime, availability
```

This gives Aobus a clear place to handle NAS/offline disks, duplicate imports,
and fast scan planning without mixing those concerns into curated metadata.

## Path Identity

The first identity layer is the normalized relative URI:

```text
normalized relative uri -> ManifestEntry -> TrackId
```

This matches the root library model and avoids expensive file hashing during
ordinary imports.

Normalization should:

- require the file to be inside the music root
- make the path relative to the music root
- lexically normalize `.` and `..`
- use a stable separator representation for stored URIs
- reject paths that escape the root
- reject normalized URI keys longer than 500 bytes

Case sensitivity should follow the platform/filesystem behavior where possible.
Do not implement broad case-folding until there is a clear cross-platform
policy.

LMDB has a default maximum key size of 511 bytes. FileManifest should reserve
headroom and reject normalized URI keys above 500 bytes during import and scan.
The error should name the path and explain that the relative path is too long
for the library index.

## FileManifest Store

Add a dedicated LMDB database:

```text
file_manifest
  key: normalized relative uri
  value: ManifestEntry
```

Unlike existing integer-key stores, `file_manifest` uses string keys. The LMDB
wrapper must support opening a database without `MDB_INTEGERKEY`; otherwise
string URI ordering and lookup behavior will be invalid. Phase 4 should add a
database construction path or options flag for non-integer keys before creating
FileManifest.

Proposed value layout:

```cpp
struct ManifestEntry final
{
  TrackId trackId{};
  std::uint32_t fileSizeLo{};
  std::uint32_t fileSizeHi{};
  std::uint32_t mtimeLo{};
  std::uint32_t mtimeHi{};
  std::uint8_t status{}; // 0: available, 1: missing, 2: error
  std::byte padding[3]{};
};
```

`fileSize` and `mtime` are split into 32-bit parts for the same 4-byte alignment
reason used by existing track layout code.

The manifest is the only source of truth for:

- URI
- file size
- modification time
- availability/missing state
- mapping from file path to track ID

## TrackColdHeader Impact

`TrackColdHeader` should not store physical file state after FileManifest is
introduced.

Remove these fields from `TrackColdHeader`:

- `fileSizeLo`
- `fileSizeHi`
- `mtimeLo`
- `mtimeHi`

Track property access that needs file state should use FileManifest-aware read
paths instead of reading those fields from the cold track payload.

Keep audio properties in `TrackColdHeader`:

- duration
- sample rate
- bitrate
- bit depth
- channels
- codec ID
- cover art reference
- track/disc numbers
- work ID
- custom metadata

This keeps `TrackStore` focused on curated and audio-domain data, while
FileManifest owns operating-system file observations.

## Store Ownership

All operations that create, delete, or move a track must update both logical and
manifest state in the same write transaction.

Required operations:

- create track and manifest entry
- find manifest entry by URI
- find track by URI
- update manifest file state
- update manifest status
- delete manifest entry
- iterate manifest entries for scan comparison

The public API should make it hard to create a track without a matching manifest
entry during normal file import. Test fixtures may still have lower-level helper
paths for synthetic tracks.

## Import Behavior

Importing a file should become idempotent for the same URI:

```text
path -> normalized uri
if uri exists in FileManifest:
  compare fileSize + mtime
  if unchanged:
    skip
  else:
    update FileManifest state
    refresh technical audio properties
    preserve curated metadata
else:
  parse file tags
  create TrackStore record
  create FileManifest entry
```

Normal import must not overwrite curated metadata from file tags when an
existing track is found.

## Scanner

Add a library scanner that builds a plan before mutating the database:

```text
ScanPlan
  new_files
  changed_files
  missing_tracks
  unchanged_tracks
  unsupported_files
  errors
```

The scanner compares:

```text
filesystem snapshot: uri -> {fileSize, mtime}
manifest snapshot:   uri -> {trackId, fileSize, mtime, status}
```

Classification:

```text
FS has uri, manifest has uri, same size+mtime -> unchanged
FS has uri, manifest has uri, changed        -> changed
FS has uri, manifest missing                 -> new
FS missing, manifest has uri                 -> missing
```

This scan requires directory walking and `stat()` calls, but no tag parsing and
no audio decoding for unchanged files.

The scan plan should be inspectable by CLI and GUI before applying changes.

## Applying a Scan Plan

Apply actions should be explicit:

- import new files
- refresh changed files
- mark missing tracks
- remove missing tracks from library
- ignore selected entries

The first implementation may apply obvious actions automatically only when
called from import:

- import new files
- skip unchanged existing files

Full scan UI should be more conservative.

## Missing Files

Missing files must not be deleted automatically.

This matters for:

- NAS mounts that are temporarily offline
- external HDDs that are unplugged
- permissions or transient filesystem errors
- slow mounts not ready during startup

FileManifest should store availability:

```text
available
missing
error
```

Playback can lazily mark a track missing when opening the file fails, but it
should not delete the track.

## Changed Files

When a known URI changes:

- update FileManifest fileSize and mtime
- refresh technical audio properties in TrackStore
- preserve curated metadata
- optionally update cover art only if it is still considered imported data

Explicit user action:

- "Re-read tags from file" may overwrite selected metadata fields

Long-term behavior:

- use field-origin tracking to update only fields that are still imported
- preserve fields edited by the user

## Rename and Move Detection

The first implementation should treat renames as:

```text
old uri -> missing
new uri -> new
```

Later, Aobus may suggest possible moves using a cheap fingerprint:

```text
fileSize + durationMs + sampleRate + first/last chunk hash
```

This should be a review tool, not an automatic merge. Full-file hashing and
acoustic fingerprinting are advanced tools and should not run during ordinary
import or scan.

## Deletion

Removing a track from the library deletes database records only. It must not
delete audio files unless Aobus later adds a separate, explicit file management
feature.

Track deletion must remove:

- hot track record
- cold track record
- FileManifest entry
- list membership references, if the current list model requires cleanup

Resource cleanup can be deferred unless reference counting exists.

## Migration

Because FileManifest is part of the first implementation of this plan, the
preferred migration is direct:

1. Create `file_manifest`.
2. For existing records, read URI, fileSize, and mtime from the old cold layout.
3. Write one manifest entry per track.
4. Rewrite cold track payloads using the slimmer `TrackColdHeader`.
5. Bump the library version.

If no released user data must be preserved, tests may create only the new schema
and migration can be limited to development fixtures.

## Verification

Tests should cover:

- duplicate import of unchanged URI is skipped
- duplicate import of changed URI refreshes file state without duplicating
- importing two different files creates two tracks and two manifest entries
- deleting a track removes its FileManifest entry
- rebuilding or migrating FileManifest from existing tracks
- scan classification for new, changed, missing, and unchanged files
- missing files are not removed automatically
- scanner rejects paths outside the music root
- fileSize and mtime are no longer stored in `TrackColdHeader`
