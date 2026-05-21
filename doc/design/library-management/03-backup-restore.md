# Library Backup and Restore

## Purpose

This document defines the export/import guarantees required by a read-only file
tag model. If Aobus keeps curated metadata in its database, users must be able
to leave, inspect, back up, and restore that data.

## Backup Principle

Exported metadata must be strong enough that users do not need audio file tag
writing as an escape hatch.

The database may be optimized and binary. The backup format should be readable,
stable, and versioned.

## Export Modes

Existing export modes should be clarified:

```text
minimum
  Data that usually does not exist in file tags:
  ratings, Aobus tags, custom metadata, lists.

metadata
  Minimum plus curated metadata:
  title, artist, album, album artist, genre, composer, work,
  year, track/disc numbers.

full
  Metadata plus observed technical properties and resources:
  duration, codec, sample rate, bit depth, channels, bitrate,
  FileManifest state, cover/resource payloads.
```

If `full` does not export resources yet, documentation and code comments must
not claim that it does.

## Required Track Fields

Every exported track must include:

- stable export ID
- URI

Metadata export must include, when present:

- title
- artist
- album
- albumArtist
- genre
- composer
- work
- year
- trackNumber
- totalTracks
- discNumber
- totalDiscs
- rating
- tags
- custom

Full export must additionally include, when present:

- durationMs
- bitrate
- sampleRate
- channels
- bitDepth
- codecId
- manifest fileSize
- manifest mtime
- manifest availability status
- cover art resource reference
- resource payload data

## Current Gaps

The current exporter/importer should be tightened before relying on it as the
primary backup path:

- `composer` is stored but not exported/imported.
- `work` is stored but not exported/imported.
- `fileSize` and `mtime` are exported in full mode but not restored.
- FileManifest does not exist yet, so exported file state is still coupled to
  the cold track payload.
- `Full` mode comments mention resource payloads, but resource export/import is
  not implemented.
- Restore currently re-reads physical file tags when files exist, then overlays
  YAML. A strict restore mode should be able to trust the backup data alone.

## Restore Modes

Import should eventually support two explicit modes.

### Restore

Restore is for backup recovery.

Rules:

- clear existing tracks and lists only after validating the file
- recreate tracks from the backup data
- do not read file tags to fill metadata
- preserve exported curated metadata exactly
- preserve list membership by remapping exported IDs to new internal IDs
- restore resources when full backup includes them

This mode is deterministic and should be the default for "Restore Library".

### Overlay

Overlay is for importing metadata into an existing or freshly scanned library.

Rules:

- match tracks by URI
- update metadata from the import file
- preserve tracks not mentioned in the import
- optionally refresh technical properties from physical files
- report unmatched import entries

This should be an explicit advanced operation, not the default backup restore
path.

## Validation

Before mutating the database, import should validate:

- YAML version
- required root sections
- unique track export IDs
- unique list export IDs
- list references to known tracks
- valid parent list references
- supported field types and numeric ranges

Only after validation should it open a write transaction and replace data.

## Sidecar Option

A future sidecar format may make portable libraries more transparent:

```text
<music-root>/.aobus/export/library.yaml
```

Sidecar export is optional. The main backup path can remain user-selected YAML.

## Verification

Tests should cover:

- metadata round-trip for all editable fields
- classical fields: composer and work
- ratings, Aobus tags, and custom metadata
- full technical properties and FileManifest state including fileSize and mtime
- list membership remapping
- missing physical files during restore
- restore without reading file tags
- resource payload round-trip once implemented
