# Aobus CLI

Status: Current behavior.

`aobus` is the scriptable command-line frontend over the shared Aobus core
library. It is built as a thin executable over `aobus-cli-lib`, so tests can
exercise the full CLI parse and callback path in-process.

## Global Contract

Global options:

- `-C, --root <dir>` selects the music root. `AOBUS_ROOT` is used when the flag
  is absent. The library database lives under `<root>/.aobus/library`.
- `-O, --output <plain|yaml|json>` selects command output. The default is
  `plain`.
- `--help-all` prints the full command tree by concatenating each command's
  normal help, including teaching footers such as the query-language
  cheatsheet.
- `--version` prints the application version.

Exit codes:

- `0`: success, including legitimately empty query results and no-op updates.
- `1`: domain failure such as an unknown id, filter/format compile error,
  import/export failure, resource IO failure, or verification failure.
- CLI usage errors keep CLI11's non-zero parse exit codes.

Stream discipline:

- stdout carries command payload only.
- stderr carries domain errors and verbose scan/apply diagnostics.

Machine-readable output uses stable shapes. YAML and JSON output is generated
from aggregate DTOs through the reflected ryml emitter, so both formats expose
the same fields. Strings are always double-quoted, numeric and boolean scalars
are emitted as scalars, empty vectors/maps are emitted as `[]`/`{}`, and empty
optionals are omitted. ryml JSON currently emits keys with a `": "` separator;
callers must parse JSON rather than matching byte-exact spacing.

Row-listing commands that naturally produce independent records may emit one
JSON object per record. Summary, mutation, detail, scan, verify, and dump
commands emit one top-level JSON/YAML object or sequence and may buffer enough
data to close that shape correctly.

## Dry-Run Contract

Every mutating CLI command accepts `--dry-run`: `init`, `scan`, `track
create/update/delete`, `tag add/remove`, `list create/update/add/remove/delete`,
and `lib import`.

Dry-run reports use the same YAML/JSON shape as the corresponding real mutation
and set `dryRun: true`; real mutations set `dryRun: false`. For writer-backed
commands, the preview executes the normal mutation path inside the LMDB write
transaction, then returns before commit and suppresses change notifications. For
`scan`, dry-run keeps the existing scan-plan preview path. For `lib import`,
dry-run parses and applies the import in the write transaction, then aborts
before commit and skips restore metadata updates.

Field-level changes are reported where the writer can compare old and new
values: track metadata reports `changes[].fields[]` with `field`, `oldValue`,
and `newValue`; tag edits report actual `addedTags`/`removedTags`; list updates
report field diffs plus `addedTrackIds` and `removedTrackIds`.

Dry-run reports do not include ids allocated by the aborted transaction. Create
previews omit `trackId` and `listId`; track creation still reports deterministic
file-derived data such as `uri`, `title`, and `artist`, while list creation
echoes the submitted draft fields. If a caller needs the created id, it must run
the real command and read the committed report.

## Scanning

`aobus init [--dry-run]` initializes the library if needed and runs the same
scan/apply path as `scan`. As with `scan --dry-run`, the runtime may create the
empty `.aobus/library` container while guaranteeing no library content is
imported.

`aobus scan [--dry-run] [--verbose]` compares files under the music root with
the manifest through runtime `LibraryScan`, which builds and applies
library scan plans without exposing scan internals to the CLI adapter.

- Plain output uses summary line `new N  changed C  moved R  missing M
  unchanged U  errors E`. Structured output emits the same counts as fields.
- `--dry-run` lists non-unchanged items and applies nothing.
- `--verbose` prints current scan, apply, and fingerprint paths to stderr.
- Per-item apply failures are reported to stderr. Transaction-level failures
  exit non-zero.
- Real scan apply reports committed relinks and unresolved missing files after
  the summary, for example `Relinked 1 moved file` or
  `2 missing files need review`.

## Tracks

`aobus track show [<id>...] [--filter <expr>] [--limit N] [--offset N]` lists
explicit tracks or tracks matching a filter. Explicit ids are current-library
operation handles and are resolved by direct lookup, not by the query language.
Structured output includes id, title, artist, album, albumArtist,
genre, composer, work, movement, year, trackNumber, trackTotal, discNumber,
discTotal, movementNumber, movementTotal, tags, duration, sampleRate, uri, and
custom metadata.

Missing fields are omitted from YAML/JSON rather than emitted as empty scalars:
empty strings, invalid dictionary ids, and numeric `0` sentinels become absent
keys. This matches query existence semantics, so `track show -O json --filter
'not $genre?'` returns records without a `genre` key.
Track ids can be supplied directly:

```bash
aobus track show 1 2 3
```

`aobus track show --format '<expr>'` formats each matching row with the query
format-expression language, for example:

```bash
aobus track show --format '$artist + " - " + $title'
```

`--format` is plain-output only and is mutually exclusive with `-O yaml/json`.
Format parse or compile errors are domain failures.

`aobus track update [--dry-run] (<id>... | --filter <expr>) [field options]`
patches track metadata through `rt::LibraryWriter::updateMetadata`. Supported
standard fields:

- `--title`, `--artist`, `--album`, `--album-artist`
- `--genre`, `--composer`, `--work`, `--movement`
- `--year`, `--track-number`, `--track-total`
- `--disc-number`, `--disc-total`
- `--movement-number`, `--movement-total`

Custom metadata uses repeatable `--set key=value` and `--unset key`. Unknown
explicit ids fail before applying. Identical-value patches succeed and report
`updated 0 of N matched track(s)`. Structured update output includes both
`matched` and `updated`; `updated` counts only tracks whose stored metadata
changed.

Other track commands:

- `track create [--dry-run] <path>` imports one audio file under the music root. Paths may
  be absolute or root-relative; imported track URIs use the same root-relative
  namespace as `scan`, and duplicate manifest entries fail.
- `track delete [--dry-run] <id>` deletes one track and removes its manifest/manual-list
  references so a later `scan` can reimport a still-present file.
- `track dump [--id <id>] [--raw]` is a plain-output debug dump only.

## Lists

`aobus list show` prints the list tree. `aobus list show <id>` prints list
details and resolved track rows through the runtime list-source path. Manual
lists resolve stored track ids within their parent source; smart lists evaluate
their filter against parent membership, so nested list results match GTK/runtime
views.

List mutation commands use `rt::LibraryWriter`:

- `list create [--dry-run] --name <name> [--filter <expr>] [--desc <text>] [--parent <id>]`
- `list update [--dry-run] <id> [--name <name>] [--desc <text>] [--filter <expr>] [--parent <id>]`
- `list add [--dry-run] <listId> <trackId>...`
- `list remove [--dry-run] <listId> <trackId>...`
- `list delete [--dry-run] <id>`

List creates and updates validate smart filters, parent existence, self-parenting,
and parent cycles before writing. Manual membership edits read the complete
current membership and submit a full replacement draft. Adding/removing
membership on a smart list is a domain failure.

`list dump [--raw]` remains an infrastructure/debug dump. `--raw` supports only
plain output.

## Tags

`aobus tag list` prints every distinct tag by descending frequency, then name.

`aobus tag add [--dry-run] <tag> (<id>... | --filter <expr>)` and
`aobus tag remove [--dry-run] <tag> (<id>... | --filter <expr>)` batch through one
`LibraryWriter::editTags` call. Missing explicit ids fail before applying.
Already-present additions and missing removals are no-op successes.

`aobus tag show <id>...` reports tags shared by every selected track. With one
id this is the track's tags; with multiple ids this is the intersection.

## Library

`aobus lib show` prints library metadata.

`aobus lib stats` reports:

- track count
- list count
- resource count and total resource bytes
- manifest entry count
- dictionary size
- distinct tag count
- on-disk library database size

`aobus lib verify` builds a scan plan without applying it and reports Changed,
Moved, Missing, and Error items. Missing or Error items exit with status `1`;
Changed and Moved items are reported but do not make verification fail.

`aobus lib relink` lists unresolved Missing/New manifest pairs and exact
audio-identity candidates. With no `--from`/`--to`, plain output prints
`missing <uri>`, `new <uri>`, and `candidate <old> -> <new>` rows. Structured
output includes `missing`, `newFiles`, and `candidates`.

`aobus lib relink --from <old-uri> --to <new-uri> [--dry-run]` explicitly binds
one missing manifest URI to one new file URI. The command validates that the
old row is unresolved, the new file is unresolved, and the audio payload
signature plus payload length match. Dry-run validates and reports without
mutating; real apply uses the normal scan executor so the track cold URI and
manifest row are rebound together.

`aobus lib import [--dry-run] <path> [--mode restore|merge]` and
`aobus lib export <path> [--mode delta|metadata|full|listOnly]` read and write
YAML library exports.

`aobus lib resource list` prints resource ids and byte sizes.

`aobus lib resource export <id> --output <file>` writes the raw resource bytes
to a file. Missing ids and file IO errors are domain failures.

`lib dump [--dict] [--manifest] [--meta] [--resources] [--raw]` remains an
infrastructure/debug dump. `--raw` supports only plain output.
