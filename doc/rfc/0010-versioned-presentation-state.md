---
id: rfc.0010.versioned-presentation-state
type: rfc
status: implemented
domain: presentation
summary: Implements versioned presentation payloads, stable textual field vocabulary, strict candidate decoding, and fail-closed installation.
depends-on: none
---
# RFC 0010: Versioned presentation state

## Disposition

Implemented on 2026-07-16 with a narrower layered design than the original shared-codec and legacy-migration proposal.

The implementation:

- makes runtime `TrackField`, `TrackSortField`, and `TrackGroupKey` choices persist through explicit stable textual ids;
- gives GTK column layouts and list-presentation preferences separate version-1 UIModel documents;
- adds a presentation-vocabulary version to the current workspace document without claiming the complete workspace envelope proposed by [RFC 0017](0017-versioned-workspace-session.md);
- decodes each versioned document with strict recursive aggregate/vector membership before semantic conversion;
- validates unknown ids, duplicates, noncanonical column dimensions, sort directions, and required presentation values before installation;
- keeps independently stored GTK groups independent on load while restoring the workspace as one all-or-nothing candidate;
- emits only canonical version-1 text tokens on explicit save; and
- rejects unversioned numeric presentation state rather than guessing at historical enum meaning.

No legacy migration was implemented because Aobus currently has no compatibility requirement for these development-era files.
Inventing and permanently governing frozen ordinal tables would add a second vocabulary without protecting shipped user data.
An explicitly rejected old document is safer than a permissive cast that can silently change meaning.

The [presentation-state reference](../reference/presentation/persisted-state.md), [workspace session-state reference](../reference/workspace/session-state.md), presentation specifications, and persistence registry linked in the promotion section own current behavior.
They supersede this proposal; this RFC records the problem, chosen boundary, and mechanisms deliberately not adopted.

## Problem

Track-table layouts and workspace presentation state persisted choices represented in code by `TrackField`, `TrackSortField`, and `TrackGroupKey` enums.
GTK column-layout files wrote numeric `TrackField` values, and workspace state reflected nested enums directly.

Enum ordinals are implementation details rather than durable identifiers.
Inserting, removing, or reordering an enumerator can make an old file decode as a different field, sort, or group without any parse failure.
That silent semantic substitution is more dangerous than rejecting the document.

Presentation payloads also lacked explicit version gates and used ordinary reflected decoding.
Missing fields could retain defaults, unknown fields could be ignored, and malformed vector elements could be skipped.
Consequently, the application could install a partially decoded presentation whose meaning no longer matched the saved object.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0008](0008-declarative-track-capability-bridge.md) established the governed runtime track-field catalog reused by the stable field-id vocabulary.

## Goals

- Give every persisted presentation payload an explicit schema or vocabulary version.
- Persist fields, sort fields, group keys, sort direction, and preset references with stable textual ids.
- Validate complete candidates before installation.
- Reject unknown versions, unknown closed-vocabulary ids, duplicate values, and invalid column state.
- Preserve valid sibling GTK groups when one independently stored group is invalid.
- Keep workspace restoration all-or-nothing under its existing transaction boundary.
- Emit one canonical representation and prevent direct numeric enum persistence at these boundaries.
- Make ownership and compatibility behavior explicit in current specifications and reference.

## Non-goals

- Changing sorting, grouping, formatting, filtering, recommendation, or column-sizing behavior.
- Defining a complete versioned, library-bound workspace envelope; RFC 0017 owns that proposal.
- Migrating Smart List expressions or defining their language version.
- Building a universal application-wide serialization framework.
- Changing the built-in presentation preset catalog.
- Adding recovery generations, automatic backups, resource budgets, or user-facing reset/export workflows.
- Preserving development-era unversioned numeric files.

## Implemented design

### Stable runtime vocabulary

Runtime owns bidirectional conversions for every persisted closed presentation choice:

- `trackFieldId` and `trackFieldFromId` use the governed `TrackFieldDefinition` ids;
- `trackSortFieldId` and `trackSortFieldFromId` cover every `TrackSortField`;
- `trackGroupKeyId` and `trackGroupKeyFromId` cover every `TrackGroupKey`; and
- workspace sort direction uses the exact strings `ascending` and `descending`.

The tables are exhaustive and case-sensitive.
Invalid enum inputs encode as an empty id, unknown strings do not decode, and tests prove uniqueness and round trip for every current value.
C++ enumerator names and ordinals are not file-format tokens.

### Layered document codecs

The implementation shares the stable vocabulary rather than forcing unrelated documents through one cross-layer codec.

Runtime privately owns `WorkspaceSessionDocument` and conversion to and from semantic `WorkspaceSessionState`.
UIModel owns `TrackColumnLayoutDocument` and `ListPresentationPreferenceDocument` plus their conversion to and from UI-local state.
GTK owns only the per-library path, group names, load fallback, logging, and atomic two-group save through `GtkLayoutStateStore`.

This keeps runtime independent of UIModel, keeps UIModel independent of GTK, and gives each persisted shape one semantic owner.

### Version gates and strict structure

The two GTK groups each carry `version: 1`.
The current workspace group carries `presentationVersion: 1`, which versions its nested presentation vocabulary but is not a claim that the complete workspace format is version 1.

All three paths use `ConfigStore::loadExact` with dedicated persistence DTOs whose member names are the exact YAML keys.
Missing members, extra members, malformed aggregate values, and malformed vector elements reject the containing group or workspace candidate rather than being seeded, ignored, or skipped.

Unsupported versions return `FormatRejected`.
No decoder interprets an absent version as an implicit legacy format.

### Semantic candidate validation

Column-layout conversion requires:

- a valid nonzero list id;
- known, unique field ids within each layout;
- unique list entries; and
- canonical fixed state (`width > 0`, `weight = -1`) or flexible state (`width = -1`, finite `weight > 0`).

List-presentation preference conversion requires valid unique list ids and nonempty presentation ids.
Presentation ids remain opaque at this layer: an unknown custom or removed preset is retained so catalog resolution can apply the documented recommendation fallback.

Workspace presentation conversion requires:

- version 1 and valid view list ids;
- a nonempty exact presentation id for every view and custom preset spec;
- known group, sort, and field tokens;
- `ascending` or `descending` direction;
- unique sort fields and unique entries in each field collection; and
- at least one visible field in a decoded version-1 presentation.

Encoding normalizes permitted live presentation defaults and deduplicates field collections before writing, while invalid enum values, duplicate sort fields, missing exact view presentations, empty ids, and invalid list ids fail with `InvalidState`.

### Installation and fallback boundaries

`GtkLayoutStateStore::load` decodes the two groups independently into temporary documents and states.
A missing or rejected group leaves the caller's corresponding seeded state unchanged; a valid sibling group can still load.
No invalid layout is installed column by column.

Workspace decoding happens before any candidate view is created.
After semantic decoding, the existing workspace transaction creates every candidate view and installs views, focus, presets, history, and revision only when all candidates succeed.
Any structural, vocabulary, or view-creation failure preserves the live workspace aggregate.

Unknown list-presentation preference ids are intentionally different from unknown closed-vocabulary tokens.
The former are extensible references and fall back through the catalog; the latter would change presentation meaning and reject the object.

### Canonical writes

Writers emit only current stable text ids and explicit version fields.
`GtkLayoutStateStore` encodes both groups before asking `ConfigStore` to replace them together in one whole-file candidate.
Workspace capture encodes one complete document before saving the `workspace` group.

GTK creates its default All Tracks view after either an accepted empty restore or a rejected restore.
It performs the automatic initial workspace checkpoint only for the accepted case, so reading an unsupported or invalid presentation document cannot overwrite it during initialization.

Encoding or replacement failure leaves the previous durable document intact under the existing [atomic replacement contract](../spec/persistence/atomic-replacement.md).
Reading rejected or unsupported data performs no write.

## Alternatives

### Keep numeric enums and never reorder them

Rejected because an ordinary source edit would remain a persistence-schema change and old bytes would not identify their intended semantics.

### Serialize C++ enum names

Rejected because source refactoring would become a file-format migration.
Explicit ids let implementation names evolve independently.

### Add only version fields

Rejected because every enum insertion or reorder would still require a migration and files would remain difficult to inspect.

### Use one universal presentation codec

Not adopted.
Workspace state belongs to runtime, column and preference state belongs to UIModel, and paths belong to GTK.
Sharing the stable token authority removes the dangerous duplication without reversing those dependencies or inventing a union document model.

### Partially salvage malformed objects

Rejected for closed presentation state because silently dropping one column, sort term, or field can change the view's meaning.
The implementation keeps only the already documented opaque-preset-id fallback.

### Freeze and migrate the unversioned numeric format

Not adopted because there is no shipped compatibility constraint for these files.
A real future migration must start from a documented versioned source schema; enum casts are never an acceptable substitute.

## Compatibility and migration

Version 1 is the first supported durable contract for these presentation payloads.
Unversioned numeric GTK layouts and workspace presentation objects are rejected and left untouched during load.
The application writes version 1 only at a later explicit save point.

Changing a stable field, sort, group, direction, or built-in preset id now requires an explicit compatibility decision and version change where necessary.
Adding a new token remains source-compatible for old version-1 documents, while an older reader rejects a document that actually uses the unknown token.

The workspace marker covers nested presentation vocabulary only.
The current document still lacks a full root schema version, library identity, exact active-view identity, resource budgets, and migration registry; RFC 0017 remains the owner of those concerns.

## Validation

The implementation is validated by:

- exhaustive stable field/sort/group id uniqueness and round-trip tests;
- direct workspace codec round trips covering grouping, ascending and descending sorts, visible fields, redundant fields, and custom presets;
- rejection tests for unsupported versions, missing/extra schema members, unknown tokens, duplicate tokens, invalid list ids, and noncanonical live state;
- UIModel codec tests for canonical column dimensions and opaque list-presentation references;
- GTK adapter tests proving canonical text output, two-group round trip, seeded-state preservation, and unversioned-format rejection;
- runtime tests proving cross-instance workspace restoration and all-or-nothing failure behavior;
- documentation validation; and
- the repository full validation gate.

## Open questions

None for this RFC.
Complete workspace versioning and bounded decoding remain tracked by RFC 0017.
A compatibility migration should be proposed only when a supported earlier version exists and its historical meaning is documented.

## Promotion plan

No proposal promotion remains.
Current behavior is owned by:

- [Presentation architecture](../architecture/presentation.md)
- [Persistence and managed-state architecture](../architecture/persistence-and-managed-state.md)
- [Workspace architecture](../architecture/workspace.md)
- [Track-list presentation](../spec/presentation/track-presentation.md)
- [Track-column layout](../spec/presentation/track-column-layout.md)
- [List presentation preference](../spec/presentation/list-preference.md)
- [Workspace session](../spec/workspace/session.md)
- [Persisted presentation state](../reference/presentation/persisted-state.md)
- [Runtime track field catalog](../reference/library/model/track-field.md)
- [Workspace session state](../reference/workspace/session-state.md)
- [Application managed-state surface](../reference/persistence/application-config.md)
