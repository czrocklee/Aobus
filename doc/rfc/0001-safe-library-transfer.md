---
id: rfc.0001.safe-library-transfer
type: rfc
status: draft
domain: library
summary: Proposes fail-closed library import validation and explicit authorization for destructive restore.
depends-on: none
---
# RFC 0001: Safe library transfer

## Problem

Library YAML restore currently combines a permissive input contract with destructive replacement semantics.
Version 1 accepts absent `library.tracks` and `library.lists` collections and ignores unknown fields, while restore clears the payload-selected target scope before rebuilding it.
A misspelled collection such as `trakcs` can therefore validate as an empty restore, clear tracks, manifest rows, and lists, and commit successfully.

The GTK workflow passes a selected file directly to `LibraryTaskService::importLibraryAsync`, whose importer uses restore by default.
The workflow does not bind a preview report to an explicit destructive confirmation before commit.

Track and list-reference URIs are normalized lexically but are not required to remain relative to the music root.
An absolute path or a path containing a surviving `..` component can therefore enter the manifest namespace and can be used when the importer reads a file baseline outside the selected library root.

The current behavior is documented by the [YAML format reference](../reference/library/format/yaml.md) and [YAML transfer specification](../spec/library/runtime/yaml-transfer.md).
The relevant implementation is owned by [`LibraryYamlImporter`](../../app/runtime/library/LibraryYamlImporter.cpp), [`LibraryTaskService`](../../app/runtime/library/LibraryTaskService.cpp), and the GTK [`LibraryImportExportWorkflow`](../../app/linux-gtk/portal/LibraryImportExportWorkflow.cpp).

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

## Proposed design

### Fail-closed schema

The next YAML format version uses a closed schema at the root, library, track, cover, list, and list-reference levels.
Every field is either recognized for that node or rejects the payload with `FormatRejected`.
Future extensions require a new format version or an explicitly reserved extension namespace.

Collection presence expresses intent:

| Payload mode | Required collections | Restore scope |
|---|---|---|
| `delta`, `metadata`, or `full` | `library.tracks` and `library.lists` | Tracks, manifest rows, and lists. |
| `listOnly` | `library.lists`; `library.tracks` is forbidden | Lists only. |

An explicitly present empty collection is valid data.
An absent required collection is a format error rather than an empty collection.

The producer emits the new version before the interactive frontend enables restore of that version.
Legacy version 1 remains readable only through merge or an explicitly named administrative legacy-restore path; it is not accepted by the normal interactive restore workflow.

### Root-contained library URIs

A shared `LibraryUri` value type owns parsing and canonicalization for Writer, scan, manifest, and YAML boundaries.
Construction rejects an empty path, an absolute path, a root name or root directory, and any normalized `..` component.
Its stored representation uses forward slashes and never begins with a separator.

Joining a `LibraryUri` to a music root is the only supported conversion to a filesystem path.
The join verifies containment after platform-specific normalization and rejects a path that escapes through a filesystem alias such as a symlink when the operation intends to read file content.
Manifest keys and list URI references are created only from a valid `LibraryUri`.

### Preview-bound authorization

Restore becomes a two-step application operation:

```text
validate + preview
  -> report + import digest + target library id/revision
  -> explicit authorization
  -> revalidate digest and target identity/revision
  -> atomic commit
```

The preview report includes the payload version and mode, target scope, tracks and lists created or deleted, ignored dangling references, and any legacy compatibility warnings.
Commit accepts a short-lived authorization object produced by preview rather than a bare path.
It fails with `Conflict` when the file digest, target library identity, or target revision no longer matches.

GTK presents the report and requires destructive confirmation before commit.
CLI restore requires an explicit restore mode and either interactive confirmation or a clearly named non-interactive confirmation flag.
Merge remains non-destructive with respect to absent payload entities but still uses strict schema and `LibraryUri` validation.

### Failure policy

Validation and preview perform no persistent mutation.
Any schema, URI, digest, identity, revision, serialization, or storage failure leaves the target library unchanged.
The success notification is emitted only after commit and change publication succeed.

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

Current Aobus exports already emit recognized fields and explicit top-level collections, so their structural content can be upgraded mechanically by the producer.
Legacy version 1 files remain usable through merge or an explicit administrative conversion command.
Normal GTK restore does not expose an unsafe compatibility bypass.

Changing the YAML compatibility surface requires updating the format version and its reference in the same implementation change.
Existing LMDB databases need no physical migration for this RFC.

## Validation

- Unit tests reject misspelled and unknown fields at every map level.
- Restore tests distinguish absent collections from explicitly empty collections.
- Tests reject POSIX absolute paths, Windows drive/root paths, UNC paths, normalized parent traversal, and root-escaping symlink targets.
- Property tests prove every accepted `LibraryUri` joins beneath its music root and round-trips through manifest keys.
- Workflow tests prove GTK cannot commit before preview confirmation.
- Preview/commit tests reject changed input bytes, a different target library id, and a changed target revision.
- Failure-injection tests prove validation and commit failures preserve all pre-import tracks, lists, manifest rows, metadata, and revision.
- CLI tests prove restore requires explicit mode and confirmation while merge remains scriptable.

## Open questions

- Should legacy version 1 restore be available only through a separate conversion command, or through a hidden administrative flag as well?
- Should root containment reject every symlink component, or only reject a resolved destination outside the root?
- Which preview size and deletion thresholds should require stronger confirmation wording?
- Should authorization expire by elapsed time in addition to digest and target-revision checks?

## Promotion plan

- Update [library architecture](../architecture/library.md) with the transfer authorization owner and shared `LibraryUri` boundary.
- Update the [YAML transfer specification](../spec/library/runtime/yaml-transfer.md) with preview-bound commit, strict failure behavior, and frontend responsibilities.
- Update the [YAML format reference](../reference/library/format/yaml.md) with the new version, closed schema, required collections, and URI grammar.
- Add a decision record if legacy version handling or symlink containment has rationale that must survive the implementation.
- Update the CLI reference and user import guide when those authorities exist in the new documentation tree.
