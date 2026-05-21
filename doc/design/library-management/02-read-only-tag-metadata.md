# Read-Only File Tags and Curated Metadata

## Purpose

This document defines Aobus's metadata ownership model. The core idea is that
audio files are source material and the Aobus database is the curated metadata
layer.

## Policy

Aobus should not write audio file tags during ordinary library editing.

File tags are used as an import source. After import, user edits are stored in
the Aobus database. This keeps playback files stable and makes metadata editing
safe for users who treat their files as archival assets.

## Rationale

Writing audio tags is risky for the target audience:

- FLAC Vorbis comments, ID3v2, MP4 atoms, DSF, APE, and other formats differ.
- Embedded cover writes can rewrite large parts of a file.
- Interrupted writes can corrupt or partially rewrite tags.
- User backup and checksum workflows may treat tag writes as unexpected file
  changes.
- Classical metadata often does not fit common file tag schemas cleanly.
- External tag editors and players may disagree about field names and
  multi-value semantics.

The safer model is:

```text
audio file tags -> initial observation
Aobus DB        -> curated source of truth
export files    -> portability and backup
```

## Editable Metadata

The following fields are curated DB metadata:

- title
- artist
- album
- album artist
- genre
- composer
- work
- year
- track number
- total tracks
- disc number
- total discs
- rating
- Aobus tags
- custom key/value metadata
- cover art resource references

Technical fields remain observed file properties:

- URI
- file size
- modification time
- duration
- codec
- sample rate
- bit depth
- channels
- bitrate

After FileManifest is introduced, URI, file size, modification time, and
availability belong to FileManifest rather than the track cold payload. Audio
technical properties remain on the track record because they describe the audio
content rather than filesystem state.

## Current Implementation Alignment

The current implementation mostly follows this model:

- `TagFile::open()` defaults to read-only.
- `TagFile` exposes `loadTrack()` but no file save interface.
- `TrackPropertiesDialog` creates a `MetadataPatch`.
- `LibraryMutationService::updateMetadata()` updates `TrackStore` records.
- Tag chips and metadata edits mutate DB state, not audio files.

This should be preserved and documented as intentional behavior.

## Refresh Semantics

When a file changes on disk, Aobus must not blindly overwrite curated metadata
from file tags.

Short-term rule:

- Refresh observed technical fields.
- Refresh file identity fields such as file size and mtime.
- Do not overwrite editable metadata unless the user explicitly chooses to
  re-read tags from the file.

Long-term rule:

Store field origin or dirty state:

```text
field origin: imported | user
```

When re-reading file tags:

- update fields still marked `imported`
- preserve fields marked `user`
- mark changed fields clearly in the UI when needed

This can be implemented later. The short-term conservative rule is enough to
avoid data loss.

## Optional Tag Writing

File tag writing is not part of the normal editing path.

If Aobus ever supports it, it should be a deliberate export action:

- user selects tracks and fields
- Aobus shows a preview/diff
- Aobus warns about file modification
- optional backup is available
- failures are reported per file

This feature should not be required for the main library model to be useful.

## Product Wording

UI and documentation should avoid implying that edits are written to files.
Prefer phrases such as:

- "Edit library metadata"
- "Export metadata"
- "Re-read tags from files"

Avoid phrases such as:

- "Save tags to file"
- "Write tags"

unless such an explicit advanced feature exists.
