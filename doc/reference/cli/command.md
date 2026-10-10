---
id: cli.command-surface
---
# CLI command reference

## Scope and version

This reference enumerates the current `aobus` command surface and machine-readable output shapes.
The surface has no explicit schema version; behavioral guarantees belong to the [CLI execution specification](../../system/frontend/cli.md).

## Code boundary

The [system architecture](../../system/overview.md) places command parsing and output in the CLI frontend.
Command registration and DTO authority live in `app/cli/`.
CLI11 help is the executable syntax authority; this document is the durable repository lookup surface kept in sync by CLI smoke tests.

## Surface

### Global options

| Option | Meaning |
| --- | --- |
| `-C, --root <dir>` | music root; falls back to `AOBUS_ROOT`, then the current directory; database is `<root>/.aobus/library` |
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
| `track update` | `(<id>... | -f, --filter <expr>) [<field-option>...] [--add-tag <tag>...] [--remove-tag <tag>...] [--dry-run]` |
| `track delete` | `<id> [--dry-run]` |
| `track dump` | `[--id <id>] [--raw]` |
| `list show` | `[<id>]` |
| `list create` | `-n, --name <name> [-f, --filter <expr>] [-d, --desc <text>] [-p, --parent <id>] [--dry-run]` |
| `list update` | `<id> [--name <name>] [--desc <text>] [--filter <expr>] [--parent <id>] [--dry-run]` |
| `list add` | `<listId> <trackId>... [--dry-run]` |
| `list remove` | `<listId> <trackId>... [--dry-run]` |
| `list order move` | `<listId> <trackId>... [--before <trackId>]` |
| `list order reset` | `<listId>` |
| `list order forget-hidden` | `<listId>` |
| `list delete` | `<id> [--descendants] [--dry-run]` |
| `list dump` | `[--raw]` |
| `tag list` | none |
| `tag show` | `(<id>... | -f, --filter <expr>)` |
| `tag add` | `<tag> (<id>... | -f, --filter <expr>) [--dry-run]` |
| `tag remove` | `<tag> (<id>... | -f, --filter <expr>) [--dry-run]` |
| `lib show` | none |
| `lib stats` | none |
| `lib verify` | none |
| `lib relink` | `[--from <old-uri> --to <new-uri>] [--dry-run]` |
| `lib fingerprint` | `--pending [--verbose]` |
| `lib export` | `(<output> | -o, --output-file <file>) [-m, --mode delta|metadata|full|listOnly]`; default mode is `full` |
| `lib import` | `(<input> | -i, --input <file>) [-m, --mode restore|merge] [--dry-run] [--confirm-destructive-restore]`; default mode is `merge` |
| `lib dump` | `[--dict] [--manifest] [--meta] [--resources] [--raw]` |
| `lib resource list` | none |
| `lib resource export` | `<id> -o, --output-file <file>` |

`--from` and `--to` must be supplied together.
`--pending` is required by `lib fingerprint`.
`--raw` dump modes support only plain output.
`--output` is reserved by the global format option, so file-destination options are `-o, --output-file`.
An explicitly empty `--filter` expression is rejected instead of silently selecting every track.
A `--filter` that matches no track is a successful no-op for the mutating commands, which report zero counts.

Track update field options are:

```text
--title --artist --album --album-artist --genre
--composer --work --movement
--recording-date <date> --credit KIND NAME ROLE --credit-scope KIND --clear-credits
--year --track-number --track-total --disc-number --disc-total
--movement-number --movement-total
--set key=value --unset key
```

`--set`, `--unset`, `--add-tag`, `--remove-tag`, `--credit`, and `--credit-scope` are repeatable.
Each of `--set`, `--unset`, `--add-tag`, and `--remove-tag` accepts one or more text arguments per occurrence; additional arguments stop at CLI11's next option, subcommand boundary, or delimiter.
The mandatory first argument may be option-like; use attached `--option=VALUE` to supply an option-like value unambiguously.
Attached values also allow further space-separated arguments. An attached empty value supplies an empty argument rather than consuming the next token.
Brackets and commas are literal text, not a list or escape codec: `[live]`, `[]`, `[[aabb]]`, and `a,b` retain their spelling.
`--set` splits only at the first `=` inside its argument: `--set '[source=manual]'` stores key `[source` with value `manual]`; `--set '[mood]=bright'` stores key `[mood]` with value `bright`.
Empty assignments and empty unset keys are rejected; empty custom values and empty tag text retain their ordinary meaning.
A `--` ending one of these unlimited vectors is consumed before ordinary option parsing resumes.
At least one field or tag option is required.
`--recording-date` accepts `YYYY`, `YYYY-MM`, or `YYYY-MM-DD`; surrounding whitespace is trimmed, an empty value explicitly clears the date, and an invalid literal is rejected with an InvalidInput error before any target is bound.
Each `--credit` consumes exactly the next three literal process arguments: `KIND NAME ROLE`, including empty or option-like name and role text. Incomplete occurrences reject the command with CLI11 argument-mismatch status `114`.
Only the space-separated triple grammar is supported; every attached `--credit=...` spelling, including boolean or empty values, is rejected with the same parse status.
**Warning:** `--credit performer Name --dry-run` stores the literal role `--dry-run`; it does not request a preview.
Place the preview flag before the triple (`--dry-run --credit performer Name ''`) or after an explicitly supplied role (`--credit performer Name '' --dry-run`).
Kind tokens are exactly `conductor`, `ensemble`, `soloist`, and `performer`.
Pass an empty argument for an absent role. Punctuation has no separator meaning: `--credit conductor 'A=B: C' ''` supplies the roleless name `A=B: C`.
Brackets have no list or escape meaning in these values: `[Smith, John]`, `[]`, and `[[aabb]]` remain literal name or role text.
Names and roles have the six ASCII whitespace characters trimmed at their boundaries and are normalized to NFC; malformed UTF-8 or a blank name rejects the whole command, and a blank role means absence.
With no scope option, all kinds are replaced. Repeated `--credit-scope KIND` options select their union; duplicates are idempotent, and there is no `all` token.
Every entry must belong to the chosen scope. Occurrences replace the complete selected segments rather than appending, preserving duplicates and supplied within-kind order; other segments stay untouched independently on each track.
`--clear-credits` clears the selected scope and cannot be combined with `--credit`. A scope without either action is rejected.
No credit action leaves credits unchanged. Recording date and unrelated fields are independent, and can share the same atomic update.
Replacing identical normalized scoped credits, or clearing an empty scope, is a no-op reported as zero updated tracks.
Unrecognized options, including `--musician`, `--clear-musicians`, `--conductor`, `--ensemble`, and `--soloist`, are rejected rather than treated as aliases.
`--set` rejects reserved keys with an InvalidInput error: the `TrackField` id vocabulary (including category projections such as `soloist`) and the runtime-owned `credits` key. The `musicians` key is not reserved.
When any tag option is present, metadata fields and tag changes commit as one atomic edit through the runtime properties mutation.
The same tag in both `--add-tag` and `--remove-tag` is rejected with an InvalidInput error.
`--dry-run` does not support tag changes yet; combined with tag options it is rejected with an InvalidInput error.
An explicitly empty `list update --filter ''` installs the identity predicate, so the List inherits all parent members.
It does not change a persisted List kind because no such kind exists.

### Structured output rules

YAML and JSON use identical field names.
Strings are double-quoted; numbers and booleans are scalars; empty containers are present; absent optionals are omitted.

Most commands emit one YAML or JSON document.
`track show -O yaml` emits a document with a `tracks` sequence, including an empty sequence when there are no results.
`track show -O json` is the exception: it emits one JSON object per track (JSON Lines), and emits no bytes for an empty result.
Each track record contains:

```text
id, title, artist, album, albumArtist, genre, composer,
work, movement, recordingDate, credits, year,
trackNumber, trackTotal, discNumber, discTotal, movementNumber,
movementTotal, tags, duration, sampleRate, uri, custom
```

Zero/empty sentinel track values are omitted.
`duration` is milliseconds.
`recordingDate` is the canonical stored precision (`YYYY`, `YYYY-MM`, or `YYYY-MM-DD`) and is omitted when absent.
`credits` is one sequence of `{name, kind, role?}` entries, including an explicit empty sequence.
It preserves duplicates and within-kind order, grouped canonically as conductor, ensemble, soloist, performer; absent roles omit the `role` key.
There are no duplicated editable `conductor`, `ensemble`, or `soloist` keys in structured output.

Mutation/administrative shapes:

| Command | Top-level fields |
| --- | --- |
| `scan` | `dryRun, new, changed, moved, missing, unchanged, errors`; dry-run adds `items[{type,uri,message?}]` |
| `track create` | `action, dryRun, trackId?, uri, title, artist` |
| `track update` | `dryRun, matched, updated, trackIds, changes, tagChanges?` |
| `track delete` | `action, dryRun, trackId, uri, title, removedFromListIds` |
| `list show` | collection rows use `id,name,description,parentId,filter,order`; the detail document wraps one row in `list` and additionally uses effective `tracks[{id,title,artist,album}]` |
| `list create` | `action, dryRun, listId?, name, parentId, filter` |
| `list update` | `action, dryRun, listId, changed, fields` |
| `list add/remove` | `action, dryRun, listId, listName, tag, changed, targetTrackIds, changes, forgottenPositionTrackIds`; `targetTrackIds` names each requested Track once; Add leaves `forgottenPositionTrackIds` empty |
| `list order move` | `action, listId, status, selectedTrackIds, beforeTrackId?` |
| `list order reset/forget-hidden` | `action, listId, status, selectedTrackIds, forgottenPositionCount` |
| `list delete` | ordinary: `action, dryRun, listId, name, forgottenPositionCount`; descendants: `action, dryRun, rootListId, deletedLists` |
| `tag list` | `tags[{name,count}]` |
| `tag show` | `trackId?` or `trackIds?`, plus `tags` |
| `tag add/remove` | `action, tag, dryRun, updated, trackIds, changes` |
| `lib show` | `libraryId, libraryVersion, flags, createdTime` |
| `lib stats` | `tracks, lists, resources, resourceBytes, manifest, dictionary, tags, diskBytes, highWaterBytes, mapBytes` |
| `lib verify` | `ok, issues[{type,uri,message?}]` |
| `lib relink` list | `missing, newFiles, candidates[{oldUri,newUri,trackId,audioPayloadLength}]` |
| `lib relink` apply | `dryRun, oldUri, newUri, trackId` |
| `lib fingerprint` | `completed, skipped, failures` |
| `lib import` | `action, path, mode, payloadVersion, payloadMode, targetScope, dryRun, tracksCreated, tracksUpdated, tracksDeleted, listsCreated, listsDeleted, danglingReferencesIgnored` |
| `lib export` | `action, path, mode` |
| `lib resource list` | `resources[{id,size}]`, where `size` is the descriptor's described length |
| `lib resource export` | `id, output, size` |
| `lib dump` | selected optional `meta`, `dictionary`, `manifest`, `resources` sections |

For `list delete --descendants`, `deletedLists` is a root-first sequence of `{listId, name, forgottenPositionCount, tagImpact?}` entries.
`forgottenPositionCount` has the same meaning as in ordinary deletion.
Only the root entry can have `tagImpact`, when its local filter is one positive writable-tag predicate.
The impact contains `tag`, the library-wide `taggedTrackCount`, `removedFromTrackCount`, and `otherListReferences[{listId,name}]` for surviving Lists that reference the tag.
CLI deletion preserves track tags, so `removedFromTrackCount` is `0`.
An absent impact is omitted, not `null`.
Preview and commit expose the same entry shape.

Other change-record nested fields are defined by the runtime mutation reply types and are emitted without CLI reinterpretation.
`track update` emits `tagChanges` only when tag options were supplied; it uses the same per-track records as `tag add`/`tag remove` changes.
In that path `updated` and `trackIds` are the sorted, deduplicated union of tracks mutated by metadata or tag changes, while `changes` stays metadata-only.
Plain output appends `added tag: <tag> to N track(s)` and `removed tag: <tag> from N track(s)` lines with the `tag add`/`tag remove` wording.
When no tag option is supplied, `updated` and `trackIds` report metadata field changes only, as before.
For `lib import`, `payloadMode` uses `delta`, `metadata`, `full`, or `listOnly`, and `targetScope` uses exact lowercase `library` or `lists`.

`lib stats` reports `resources` as the number of descriptor rows and `resourceBytes` as the summed described length of the descriptors tracks currently reference, counting each reachable descriptor once however many tracks name it.
The two figures disagreeing is the normal state of a rescanned library: descriptor rows are never deleted, so a cover a file no longer carries keeps its row while leaving `resourceBytes`.
Neither figure counts stored bytes, because the library stores no cover content.

`lib stats` reports three separate byte figures for the database and they answer different questions.
`diskBytes` is what database-owned files allocate, excluding frontend state and unrelated descendants. It counts allocation rather than file length so a sparse data file is not reported as the whole map; the [database reference](../library/storage/database.md) owns the file inventory.
`mapBytes` is the capacity the environment may grow into before a mutation runs out of room.
`highWaterBytes` is how much of that capacity the environment has ever needed; deleting rows returns their pages for reuse without lowering it, so it is a peak rather than a measure of live data.

### Plain scan/status text

Scan summary is:

```text
new N  changed C  moved R  missing M  unchanged U  errors E
```

A non-dry-run `scan` or `init` follows the plan summary with apply lines for skipped, relinked, and missing-review counts, plus an `N items failed to apply` line (`1 item failed to apply` for one item) when items failed.
Any apply failure also fails the command with `scan apply failed: ...` on stderr and exit `1`, for plain and structured output alike.

Fingerprint summary is `fingerprinted N  skipped N  failed N`.
Tag list is descending frequency then name.
`tag show` returns the intersection across all supplied or filter-matched tracks.

Plain `lib import` output identifies whether the operation is a preview, then prints payload version, payload mode, target scope, track/list create-update-delete counts, and ignored dangling references.

### Streams and exit codes

| Channel/status | Contract |
| --- | --- |
| stdout | command payload |
| stderr | domain errors and `--verbose` progress |
| exit `0` | success, including empty/no-op |
| exit `1` | domain or internal failure |
| other nonzero | CLI11 usage/parse failure |

For saved-order commands, runtime `Stale` maps to a `Conflict` domain failure and runtime `Unavailable` maps to an `InvalidState` domain failure.
Both write the error to stderr, emit no success document, and exit `1`; only `Applied` and `NoOp` reach the normal output path.

## Validation rules

- `track show --format` is mutually exclusive with YAML/JSON.
- `track update` applies field options and tag options as one atomic edit; the same tag in `--add-tag` and `--remove-tag` is rejected, and `--dry-run` is rejected when tag options are present.
- `track update` rejects credit arity, scope, kind, text, and clear/replacement conflicts, invalid recording-date literals, and reserved keys in `--set`, all before mutation.
- `track update` change records use the runtime field names: `recordingDate` values are canonical date strings, and `credits` values are diagnostics-only text, never replacement input.
- Explicit missing ids fail before mutation.
- List parent existence, self-parenting, and cycles are rejected.
- `list add/remove` require a List whose complete local expression is one positive tag predicate; compound, negated, or non-tag predicates are not directly writable.
- `list remove` removes that global tag from the target tracks and also forgets their saved positions in the List; plain output states both effects.
- `list order move` binds the current effective sequence, preserves selected relative order, inserts it before the optional anchor, and moves it to the bottom when the anchor is omitted.
- `list order reset` forgets all saved positions in the target List, including currently hidden tracks; it leaves other Lists' orders unchanged.
- `list order forget-hidden` forgets only positions outside current effective membership, preserving the saved relative order of visible tracks.
  Neither command changes track tags.
  Their reports include `selectedTrackIds: []` and the number of forgotten positions; an unchanged order is a successful no-op.
- No `list order` subcommand accepts `--dry-run`; supplying it is a usage failure.
- The saved-order runtime may return `Applied`, `NoOp`, `Stale`, or `Unavailable`.
  CLI output uses `applied` or `no-op`; `Stale` fails as `Conflict`, `Unavailable` fails as `InvalidState`, and none of NoOp/Stale/Unavailable advances library revision.
- Ordinary List deletion rejects a List with descendants; `--descendants` explicitly selects complete-subtree deletion, and `--dry-run` reports the same subtree without committing.
- `lib verify` fails only for Missing or Error, while still reporting Changed/Moved.
- `lib resource list` reports each descriptor without reading content; `lib dump --resources` additionally prints each digest.
- `lib resource export` reads content through the same source walk interactive consumers use, with no size ceiling, so it can write a cover that exceeds the interactive limit.
- `lib resource export` fails with `resource not found` for an id with no descriptor row, `resource not available` when no cache entry or referencing file could reproduce the content, and for file IO, which includes an output below a directory that does not exist; it writes nothing in every failure case, replaces an existing output only once the complete content is on hand, and creates no directory.
- Create dry-runs omit transaction-allocated ids.
- `lib import --mode restore --dry-run` validates and previews without requiring confirmation.
- A committing restore requires `--confirm-destructive-restore`; merge requires neither that flag nor an interactive prompt.
- Import apply fails when the YAML bytes or target library identity/revision change after its in-process preview.

## Compatibility and versioning

The command surface and DTOs are unversioned.
Subtree deletion entries use `forgottenPositionCount` and `tagImpact` rather than the former undocumented `orderTrackIdCount` and `optTagImpact` keys; there is no compatibility alias.
Scripts reading the former keys must use the new names.
Any syntax or field change requires updating this reference and `CliSmokeTest` in the same change.
Library YAML and database versioning are independent.
There is no separate CLI protocol version or migration layer.

## Examples

```bash
aobus -C /music track show --filter '$artist == "Miles Davis"' -O json
aobus track update 42 --recording-date 1981-05-12 --credit soloist 'Glenn Gould' Piano --credit conductor 'Leonard Bernstein' ''
aobus track update 42 --credit-scope performer --clear-credits
aobus track update 42 --dry-run --credit performer 'Preview only' ''
aobus track update 42 --composer "J. S. Bach" --set source=manual --dry-run
aobus track update 12 --genre Jazz --add-tag favourite --remove-tag inbox
aobus lib export backup.yaml --mode full
aobus lib resource export 3 --output-file cover.jpg
aobus -O json lib import backup.yaml --mode restore --dry-run
aobus lib import backup.yaml --mode restore --confirm-destructive-restore
```

For a roleless credit in Windows Command Prompt, pass a double-quoted empty argument:

```bat
aobus.exe -C "C:\Music" track update 42 --credit conductor "Leonard Bernstein" ""
```

Windows PowerShell 5.1 drops empty strings in ordinary native-command argument passing.
Use its stop-parsing token for a literal native invocation that preserves the empty role:

```powershell
aobus.exe --% -C "C:\Music" track update 42 --credit conductor "Leonard Bernstein" ""
```

After `--%`, the example uses literal arguments, not PowerShell variables or expressions.
The CLI cannot restore an empty argument removed by the invoking shell; missing arguments reject the command rather than partially applying it.

## Implementation authority

- [`Run.cpp`](../../../app/cli/Run.cpp) owns global options and exits.
- [`TrackCommand.cpp`](../../../app/cli/TrackCommand.cpp), [`ListCommand.cpp`](../../../app/cli/ListCommand.cpp), [`TagCommand.cpp`](../../../app/cli/TagCommand.cpp), [`ScanCommand.cpp`](../../../app/cli/ScanCommand.cpp), and [`LibCommand.cpp`](../../../app/cli/LibCommand.cpp) own the listed surface and DTOs.

## Test authority

- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) protects the command tree and representative exact shapes.
- [`OutputTest.cpp`](../../../test/unit/cli/OutputTest.cpp) protects encoding rules.
- [`ListDestructiveCommandTest.cpp`](../../../test/unit/cli/ListDestructiveCommandTest.cpp) protects subtree preview/commit, dependent-List refusal and saved-order cleanup.
- [`TrackPerformanceMetadataCommandTest.cpp`](../../../test/unit/cli/TrackPerformanceMetadataCommandTest.cpp) protects recording-date and Credits show/update options, literal fixed-triple arguments, native argv dry-run placement, attached-form rejection, exact arity, option context, scope/clear/conflict/rejection rules, and structured sequence output.
- [`TrackLiteralMetadataCommandTest.cpp`](../../../test/unit/cli/TrackLiteralMetadataCommandTest.cpp) protects literal tag/custom operands, attached and multiargument forms, contextual recognition, and atomic rejection.

## Related documents

- [CLI execution specification](../../system/frontend/cli.md)
- [Predicate language reference](../query/predicate-language.md)
- [Format language reference](../query/format-language.md)
- [Library YAML format reference](../library/format/yaml.md)
