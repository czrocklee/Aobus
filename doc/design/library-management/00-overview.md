# Library Management Overview

## Purpose

This document set defines the intended direction for Aobus library management.
It turns the current root-based library model into an explicit product and
architecture choice, while addressing practical issues such as NAS libraries,
read-only music folders, slow external disks, duplicate imports, and reliable
backup/restore.

The design keeps Aobus oriented toward carefully maintained music collections:
a library is a concrete collection rooted at one music directory, not a virtual
aggregation of unrelated media locations.

## Design Position

Aobus should keep the Git-like library model as the primary mental model:

```text
one library profile -> one music root -> one active database
```

The music root remains the identity boundary for paths, playback, scanning, and
user expectations. Database storage may be portable or external, but the
library still represents one rooted collection.

## Goals

- Preserve single-root library semantics and relative track URIs.
- Support portable libraries where the database lives with the music root.
- Support external databases for NAS, read-only roots, and slow external disks.
- Keep file tags read-only by default.
- Treat the Aobus database as the curated metadata layer.
- Provide reliable metadata export/import before considering file tag writing.
- Add FileManifest-based duplicate prevention for ordinary import workflows.
- Add a scanner that detects new, changed, missing, and unchanged files.
- Avoid destructive actions when a NAS or external disk is temporarily offline.

## Non-Goals

- Do not replace the root library model with multi-source aggregation.
- Do not federate multiple databases into one merged UI.
- Do not write metadata back into audio files as part of normal editing.
- Do not automatically organize, rename, or move user audio files.
- Do not run full-file hashing or acoustic fingerprinting during normal import.

These capabilities may be revisited as explicit advanced tools, but they should
not shape the first implementation of library management.

## Current State

The current implementation already aligns with much of this direction:

- `library::MusicLibrary` is constructed from a single root path.
- Track URIs are stored relative to the root during import.
- Tag files are opened read-only by default.
- Metadata edits update `TrackStore` records rather than audio files.
- YAML export/import exists for logical library data.

The main gaps are:

- The database location is currently tied to the library root.
- There is no library profile manifest separating music root from database path.
- Import is append-only and does not prevent duplicate tracks for the same URI.
- There is no scanner plan for new, changed, missing, or renamed files.
- Backup/export is not yet complete enough to be the only user escape hatch.
- Export/import currently misses some classical-critical fields and resource
  data.

## Document Map

- `01-library-profiles-and-storage.md`
  Defines portable and external database modes.
- `02-read-only-tag-metadata.md`
  Defines the metadata source-of-truth model and file tag policy.
- `03-backup-restore.md`
  Defines export/import expectations required by a read-only tag model.
- `04-file-manifest-scan-dedup.md`
  Defines FileManifest, duplicate prevention, scan planning, missing files, and
  refresh rules.
- `05-implementation-plan.md`
  Breaks the work into reviewable implementation phases.
- `06-smart-restoration-implementation-plan.md`
  Defines the one-shot implementation plan for smart export/import, real
  merge import, list-only synchronization, delta payloads, and cover art
  resource restoration.
