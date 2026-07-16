---
id: cli.command-surface
type: reference
status: current
domain: presentation
summary: Enumerates the Aobus CLI global options, commands, arguments, output fields, dry-run coverage, streams, and exit codes.
---
# CLI command reference

## Scope and version

This reference enumerates the current `aobus` command surface and machine-readable DTOs.
The surface has no explicit schema version; behavioral guarantees belong to the [CLI execution specification](../../spec/cli/execution.md).

## Code boundary

Command registration and DTO authority live in `app/cli/`.
CLI11 help is the executable syntax authority; this document is the durable repository lookup surface kept in sync by CLI smoke tests.

## Surface

### Global options

| Option | Meaning |
| --- | --- |
| `-C, --root <dir>` | music root; falls back to `AOBUS_ROOT`; database is `<root>/.aobus/library` |
| `-O, --output <plain|yaml|json>` | output format; default `plain`; names are case-insensitive |
| `--help-all` | recursive complete command help |
| `--version` | application version |

Exactly one top-level command is required.

### Command inventory

| Command | Arguments/options |
| --- | --- |
| `init` | `[--dry-run]` |
| `scan` | `[--dry-run] [--verbose] [--defer-fingerprint]` |
| `track show` | `[<id>...] [-f, --filter <expr>] [-l, --limit N] [-o, --offset N] [--format <expr>]` |
| `track create` | `<path> [--dry-run]` |
| `track update` | `(<id>... | -f, --filter <expr>) <field-option>... [--dry-run]` |
| `track delete` | `<id> [--dry-run]` |
| `track dump` | `[--id <id>] [--raw]` |
| `list show` | `[<id>]` |
| `list create` | `-n, --name <name> [-f, --filter <expr>] [-d, --desc <text>] [-p, --parent <id>] [--dry-run]` |
| `list update` | `<id> [--name] [--desc] [--filter] [--parent] [--dry-run]` |
| `list add` | `<listId> <trackId>... [--dry-run]` |
| `list remove` | `<listId> <trackId>... [--dry-run]` |
| `list delete` | `<id> [--dry-run]` |
| `list dump` | `[--raw]` |
| `tag list` | none |
| `tag show` | `<id>...` |
| `tag add` | `<tag> (<id>... | -f, --filter <expr>) [--dry-run]` |
| `tag remove` | `<tag> (<id>... | -f, --filter <expr>) [--dry-run]` |
| `lib show` | none |
| `lib stats` | none |
| `lib verify` | none |
| `lib relink` | `[--from <old-uri> --to <new-uri>] [--dry-run]` |
| `lib fingerprint` | `--pending [--verbose]` |
| `lib export` | `(<output> | -o, --output <file>) [-m, --mode delta|metadata|full|listOnly]`; default mode is `full` |
| `lib import` | `(<input> | -i, --input <file>) [-m, --mode restore|merge] [--dry-run] [--confirm-destructive-restore]`; default mode is `merge` |
| `lib dump` | `[--dict] [--manifest] [--meta] [--resources] [--raw]` |
| `lib resource list` | none |
| `lib resource export` | `<id> -o, --output <file>` |

`--from` and `--to` must be supplied together.
`--pending` is required by `lib fingerprint`.
`--raw` dump modes support only plain output.

Track update field options are:

```text
--title --artist --album --album-artist --genre
--composer --conductor --ensemble --work --movement --soloist
--year --track-number --track-total --disc-number --disc-total
--movement-number --movement-total
--set key=value --unset key
```

`--set` and `--unset` are repeatable.
At least one field option is required.
An explicitly empty `list update --filter ''` converts a Smart List to manual.

### Structured output rules

YAML and JSON use identical field names.
Strings are double-quoted; numbers and booleans are scalars; empty containers are present; absent optionals are omitted.

Track records are wrapped as `tracks` and contain:

```text
id, title, artist, album, albumArtist, genre, composer, conductor,
ensemble, work, movement, soloist, year, trackNumber, trackTotal,
discNumber, discTotal, movementNumber, movementTotal, tags, duration,
sampleRate, uri, custom
```

Zero/empty sentinel track values are omitted.
`duration` is milliseconds.

Mutation/administrative shapes:

| Command | Top-level fields |
| --- | --- |
| `scan` | `dryRun, new, changed, moved, missing, unchanged, errors`; dry-run adds `items[{type,uri,message?}]` |
| `track create` | `action, dryRun, trackId?, uri, title, artist` |
| `track update` | `dryRun, matched, updated, trackIds, changes` |
| `track delete` | `action, dryRun, trackId, uri, title, removedFromListIds` |
| `list show` | `lists[...]` or `list{ id,name,description,type,parentId,filter?,tracks[...] }` |
| `list create` | `action, dryRun, listId?, name, type, parentId, filter?` |
| `list update` | `action, dryRun, listId, changed, fields, addedTrackIds, removedTrackIds` |
| `list add` | `action, dryRun, listId, changed, insertionIndex, insertedTrackIds, duplicateRequest, alreadyPresent, missingTrack` |
| `list remove` | `action, dryRun, listId, changed, removedTrackIds, duplicateRequest, notPresent` |
| `list delete` | `action, dryRun, listId, name, type, trackCount` |
| `tag list` | `tags[{name,count}]` |
| `tag show` | `trackId?` or `trackIds?`, plus `tags` |
| `tag add/remove` | `action, tag, dryRun, updated, trackIds, changes` |
| `lib show` | `libraryId, libraryVersion, flags, createdTime` |
| `lib stats` | `tracks, lists, resources, resourceBytes, manifest, dictionary, tags, diskBytes` |
| `lib verify` | `ok, issues[{type,uri,message?}]` |
| `lib relink` list | `missing, newFiles, candidates[{oldUri,newUri,trackId,audioPayloadLength}]` |
| `lib relink` apply | `dryRun, oldUri, newUri, trackId` |
| `lib fingerprint` | `completed, skipped, failures, cancelled` |
| `lib import` | `action, path, mode, payloadVersion, payloadMode, targetScope, dryRun, tracksCreated, tracksUpdated, tracksDeleted, listsCreated, listsDeleted, danglingReferencesIgnored` |
| `lib export` | `action, path, mode` |
| `lib resource list` | `resources[{id,size}]` |
| `lib resource export` | `id, output, size` |
| `lib dump` | selected optional `meta`, `dictionary`, `manifest`, `resources` sections |

Change-record nested fields are defined by the runtime mutation reply types and are emitted without CLI reinterpretation.
For `lib import`, `payloadMode` uses `delta`, `metadata`, `full`, or `listOnly`, and `targetScope` uses exact lowercase `library` or `lists`.

### Plain scan/status text

Scan summary is:

```text
new N  changed C  moved R  missing M  unchanged U  errors E
```

Fingerprint summary is `fingerprinted N  skipped N  failed N`.
Tag list is descending frequency then name.
`tag show` returns the intersection across all supplied tracks.

Plain `lib import` output identifies whether the operation is a preview, then prints payload version, payload mode, target scope, track/list create-update-delete counts, and ignored dangling references.

### Streams and exit codes

| Channel/status | Contract |
| --- | --- |
| stdout | command payload |
| stderr | domain errors and `--verbose` progress |
| exit `0` | success, including empty/no-op |
| exit `1` | domain or internal failure |
| other nonzero | CLI11 usage/parse failure |

## Validation rules

- `track show --format` is mutually exclusive with YAML/JSON.
- Explicit missing ids fail before mutation.
- List parent existence, self-parenting, and cycles are rejected.
- Manual membership commands reject Smart Lists.
- `lib verify` fails only for Missing or Error, while still reporting Changed/Moved.
- `lib resource export` fails for missing id or file IO.
- Create dry-runs omit transaction-allocated ids.
- `lib import --mode restore --dry-run` validates and previews without requiring confirmation.
- A committing restore requires `--confirm-destructive-restore`; merge requires neither that flag nor an interactive prompt.
- Import apply fails when the YAML bytes or target library identity/revision change after its in-process preview.

## Compatibility and versioning

The command surface and DTOs are unversioned.
Any syntax or field change requires updating this reference and `CliSmokeTest` in the same change.
Library YAML and database versioning are independent.
[RFC 0029](../../rfc/0029-versioned-cli-automation-protocol.md) proposes a separate CLI automation version and migration from these legacy top-level DTOs.

## Examples

```bash
aobus -C /music track show --filter '$artist == "Miles Davis"' -O json
aobus track update 42 --composer "J. S. Bach" --set source=manual --dry-run
aobus lib export backup.yaml --mode full
aobus -O json lib import backup.yaml --mode restore --dry-run
aobus lib import backup.yaml --mode restore --confirm-destructive-restore
```

## Implementation authority

- [`Run.cpp`](../../../app/cli/Run.cpp) owns global options and exits.
- [`TrackCommand.cpp`](../../../app/cli/TrackCommand.cpp), [`ListCommand.cpp`](../../../app/cli/ListCommand.cpp), [`TagCommand.cpp`](../../../app/cli/TagCommand.cpp), [`ScanCommand.cpp`](../../../app/cli/ScanCommand.cpp), and [`LibCommand.cpp`](../../../app/cli/LibCommand.cpp) own the listed surface and DTOs.

## Test authority

- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) protects the command tree and representative exact shapes.
- [`OutputTest.cpp`](../../../test/unit/cli/OutputTest.cpp) protects encoding rules.

## Related documents

- [CLI execution specification](../../spec/cli/execution.md)
- [Predicate language reference](../query/predicate-language.md)
- [Format language reference](../query/format-language.md)
- [Library YAML format reference](../library/format/yaml.md)
- [RFC 0029: versioned CLI automation protocol](../../rfc/0029-versioned-cli-automation-protocol.md)
