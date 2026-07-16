---
id: rfc.0001.safe-library-transfer
type: rfc
status: implemented
domain: library
summary: Introduced a version-2 closed transfer schema, root-contained LibraryUri, and preview-bound destructive restore authorization.
depends-on: none
---
# RFC 0001: Safe library transfer

## Disposition

Implemented on 2026-07-16 as a deliberately strict replacement surface.

The implementation provides:

- one version-2 closed schema with explicit payload collections and no version-1 reader, alias, conversion command, or restore bypass;
- shared `LibraryUri` validation at YAML, manifest, Writer, scanner, and actual file-access boundaries, including escaping and unresolved symlink rejection;
- a move-only `LibraryImportPlan` produced by full validation and an uncommitted preview;
- exact source-byte, target runtime, library identity, and committed-revision revalidation before one atomic apply;
- GTK preview presentation and explicit positive confirmation before restore;
- CLI merge by default, report-producing restore dry-run, and an explicit destructive restore flag; and
- report metadata for payload version, mode, scope, all create/update/delete counts, and ignored dangling references.

The implementation intentionally has no compatibility or migration path for earlier YAML files.
This repository has no deployed interchange compatibility constraint, so retaining a permissive reader would preserve the unsafe semantics this RFC removes.

Current behavior is owned by the [library architecture](../architecture/library.md), [YAML transfer specification](../spec/library/runtime/yaml-transfer.md), [YAML format reference](../reference/library/format/yaml.md), [CLI command reference](../reference/cli/command.md), and [backup and restore guide](../user/backup-and-restore.md).
Those authorities supersede this proposal; this RFC retains the original problem and rejected alternatives.

## Problem

Before this RFC, library YAML restore combined a permissive input contract with destructive replacement semantics.
Version 1 accepts absent `library.tracks` and `library.lists` collections and ignores unknown fields, while restore clears the payload-selected target scope before rebuilding it.
A misspelled collection such as `trakcs` can therefore validate as an empty restore, clear tracks, manifest rows, and lists, and commit successfully.

The GTK workflow passed a selected file directly to `LibraryTaskService::importLibraryAsync`, whose importer used restore by default.
The workflow does not bind a preview report to an explicit destructive confirmation before commit.

Track and list-reference URIs were normalized lexically but were not required to remain relative to the music root.
An absolute path or a path containing a surviving `..` component can therefore enter the manifest namespace and can be used when the importer reads a file baseline outside the selected library root.

At proposal time, those behaviors were implemented by `LibraryYamlImporter`, `LibraryTaskService`, and the GTK `LibraryImportExportWorkflow`.
Those same boundaries now contain the strict replacement described below; current behavior is owned by the promoted authorities listed in the disposition.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Make omission, misspelling, or forward-unknown fields unable to authorize destructive deletion.
- Admit only canonical library-root-relative URIs at every import and manifest boundary.
- Require a user-visible preview and explicit authorization before an interactive restore changes durable state.
- Detect when either the import file or target library changes between preview and commit.
- Keep non-interactive administration possible through an explicit, auditable command surface.
- Preserve atomic failure behavior for a committed restore.

## Non-goals

- Replace YAML as the portable interchange format.
- Change the host-local LMDB record layout.
- Treat imported paths as a general-purpose filesystem access mechanism.
- Add automatic conflict resolution for merge imports.
- Define database backup, migration, or corruption recovery policy.

## Implemented design

### Fail-closed schema

YAML format version 2 uses a closed schema at the root, library, track, cover, list, and list-reference levels.
Every field is either recognized for that node or rejects the payload with `FormatRejected`.
Future extensions require a new format version or an explicitly reserved extension namespace.

Collection presence expresses intent:

| Payload mode | Required collections | Restore scope |
|---|---|---|
| `delta`, `metadata`, or `full` | `library.tracks` and `library.lists` | Tracks, manifest rows, and lists. |
| `listOnly` | `library.lists`; `library.tracks` is forbidden | Lists only. |

An explicitly present empty collection is valid data.
An absent required collection is a format error rather than an empty collection.

The producer emits version 2 and the importer accepts version 2 only.
There is no legacy merge, administrative restore, or conversion path.

### Root-contained library URIs

A shared `LibraryUri` value type owns parsing and canonicalization for Writer, scan, manifest, and YAML boundaries.
Construction rejects an empty path, an absolute path, a root name or root directory, any normalized `..` component, and text longer than the manifest's 500-byte key limit.
Its unique stored representation uses forward slashes, has no trailing separator, and never begins with a separator.

Joining a `LibraryUri` to a music root is the only supported conversion to a filesystem path.
The join verifies containment after platform-specific normalization and rejects a path that escapes through a filesystem alias such as a symlink.
It also rejects a dangling symlink component instead of accepting the unresolved lexical spelling as evidence of containment.
The root and an ordinary non-symlink suffix may be absent, preserving first-run metadata restore before the audio directory exists.
Runtime consumers resolve again at the actual playback, read-model, fingerprint, and scan-apply boundary rather than trusting an earlier import or scan check.
Manifest keys and list URI references are created only from a valid `LibraryUri`.

### Preview-bound authorization

Restore becomes a two-step application operation:

```text
validate + preview
  -> report + exact source bytes + target runtime/library id/revision
  -> explicit authorization
  -> revalidate source bytes and target runtime/identity/revision
  -> atomic commit
```

The preview report includes payload version and mode, target scope, track create/update/delete counts, list create/delete counts, and ignored dangling references.
Every commit attempt consumes a one-shot authorization object produced by preview rather than a bare path, including an attempt that fails before commit.
It fails with `Conflict` when source bytes, runtime identity, target library identity, or target revision no longer match.

GTK presents the report and requires destructive confirmation before commit.
CLI restore requires an explicit restore mode and the clearly named non-interactive `--confirm-destructive-restore` flag.
Merge remains non-destructive with respect to absent payload entities but still uses strict schema and `LibraryUri` validation.

### Failure policy

Validation and preview perform no persistent mutation.
Any schema, URI, source-byte, identity, revision, serialization, or storage failure leaves the target library unchanged.
After durable commit, publication enqueue or observer failure follows the shared committed-publication policy: the mutation remains committed and the runtime becomes terminal `Faulted` rather than returning a retryable import failure.
Frontend completion occurs only after the callback-executor hop, and cancellation or owner destruction can suppress presentation without undoing committed state.

## Alternatives

### Keep permissive parsing and add a confirmation dialog

Confirmation reduces accidental activation but still presents a misleading preview when a misspelled field is silently ignored.
It also leaves non-interactive callers and root escape unresolved.

### Require collections only in restore mode

This closes the most destructive omission path while retaining an open schema.
Unknown track or list fields could still silently lose data, so it is weaker than a versioned closed schema.

### Change version 1 validation in place

This is simpler and would make all callers safer immediately, but it changes a documented compatibility contract without a format-version signal.
A new version makes the strict acceptance boundary explicit and permits a deliberate legacy policy.

### Canonicalize with `std::filesystem::path` at every caller

Distributed checks are likely to drift across platforms and import paths.
A value type makes an invalid URI unrepresentable after the boundary.

## Compatibility and migration

There is no YAML compatibility or migration surface.
Version 1 is rejected in restore and merge, and the CLI and GTK expose no bypass or converter.
Users recreate a portable file with the current exporter when they still have its source library.

The host-local LMDB layout is unchanged and needs no physical migration.
Future interchange changes update the format version and reference in the same implementation change.

## Validation

- Unit tests reject misspelled and unknown fields at every map level.
- Restore tests distinguish absent collections from explicitly empty collections.
- Tests reject POSIX absolute paths, Windows drive/root paths, UNC paths, normalized parent traversal, control characters, and root-escaping or dangling symlink targets.
- Table-driven tests prove accepted `LibraryUri` values have one bounded canonical spelling, resolve beneath their root, and manifest operations reject non-canonical keys.
- Schema tests reject duplicate canonical track URIs, duplicate custom keys, invalid or cyclic lists, strict-base64 suffixes, and values exceeding core storage limits.
- Workflow tests prove GTK cannot commit before preview confirmation.
- Preview/commit tests reject changed input bytes, a different target library id, and a changed target revision.
- Cancellation tests prove cancellation before admission leaves maintenance untouched and cancellation after commit cannot suppress mandatory callback completion.
- Failure-injection tests prove validation and commit failures preserve all pre-import tracks, lists, manifest rows, metadata, and revision.
- CLI tests prove restore requires explicit mode and confirmation while merge remains scriptable.

## Open questions

None for the implemented boundary.
Resolved destinations outside the root are rejected and in-root symlinks use their canonical target identity.
The library tree is not an adversarial filesystem sandbox: consumers re-resolve at each access boundary, but assume path components are not concurrently replaced between that resolution and the operating-system open.
Whole-transfer byte budgets, streaming, and bounded prepared-memory execution remain owned by [RFC 0004](0004-scalable-library-tasks.md); version 2 does not invent a ceiling that would break exporter/importer round trips.

## Promotion plan

Implemented current behavior is owned by:

- [Library architecture](../architecture/library.md)
- [Library YAML transfer specification](../spec/library/runtime/yaml-transfer.md)
- [Library YAML format reference](../reference/library/format/yaml.md)
- [CLI command reference](../reference/cli/command.md)
- [Back up and restore library data](../user/backup-and-restore.md)

No separate decision record is needed because this RFC retains the rationale for rejecting compatibility and migration.
