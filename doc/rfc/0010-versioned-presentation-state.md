---
id: rfc.0010.versioned-presentation-state
type: rfc
status: draft
domain: presentation
summary: Proposes versioned presentation persistence with stable identifiers, frozen legacy migration, and fail-closed decoding.
depends-on: none
---
# RFC 0010: Versioned presentation state

## Problem

Track-table layouts and workspace presentation state persist choices that are represented in code by enums such as `TrackField`, `TrackSortField`, and `TrackGroupKey`.
At least the GTK column-layout format writes the numeric value of `TrackField`; legacy fixtures contain values such as `field: 0`.
The broader workspace state also embeds list presentation configuration and custom presentation presets without a documented, explicitly versioned persistence contract.

Enum ordinals are implementation details, not durable identifiers.
Inserting, removing, or reordering an enumerator can make a valid old file decode as a different field, sort, or group without producing an error.
That is more dangerous than a rejected configuration because the application silently presents the wrong data.

Stable preset ids already exist, but there is no single persistence boundary that defines the schema version, stable ids for all nested presentation choices, validation, unknown-value behavior, and migration from the numeric representation.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0008](0008-declarative-track-capability-bridge.md) may generate the track-field portion of the stable-id codec; otherwise this RFC must establish an equivalent governed owner.

## Goals

- Give every persisted presentation payload an explicit schema version.
- Persist track fields, sort fields, group keys, and preset references with stable textual ids rather than enum ordinals.
- Decode the existing numeric layouts through frozen legacy mappings that do not change when current enums change.
- Validate complete semantic objects before installing them.
- Fail closed to a documented default instead of partially applying ambiguous or invalid presentation state.
- Preserve valid neighboring views and presets when one independently stored object is invalid.
- Give serialization and migration one code owner shared by GTK layout and workspace persistence.
- Make compatibility behavior testable with golden fixtures.

## Non-goals

- Changing current sorting, grouping, formatting, filtering, or column-layout behavior.
- Defining the library database schema or playback persistence.
- Making C++ enum names part of the file format.
- Building a universal application-wide serialization framework.
- Migrating Smart List expressions; their language and persistence belong to the query system.
- Changing the built-in presentation preset catalog.

## Proposed design

### Versioned envelopes

Every independently persisted presentation document carries a version at its root.
The concrete encoding may remain the current configuration encoding, but its logical shape is explicit:

```text
version: 1
layouts:
  - view: library
    columns:
      - field: track.title
        width: 320
        visible: true
```

Workspace presentation state uses the same versioned value vocabulary for each open view and custom preset:

```text
version: 1
views:
  - presentation:
      preset: album
      group: album
      sort:
        field: track.disc-number
        direction: ascending
```

The final field names follow the existing store conventions, but the version and stable-id requirements are normative.
Nested structures do not infer a version from the application release.

### Stable persisted identifiers

Define stable textual ids for every persisted choice:

- track fields use ids such as `track.title`, `track.album`, and `track.duration`;
- sort fields and group keys use their own stable namespaces;
- built-in and custom presets continue to use stable preset ids;
- sort direction and other closed choices use documented strings rather than integer casts.

The ids are format tokens, not generated spellings of C++ enumerators.
Changing an id is a schema migration even when the corresponding enum is merely renamed.

The runtime owns typed conversion between stable ids and enum values.
Presentation persistence calls that codec; it does not maintain a second switch table.
[RFC 0008](0008-declarative-track-capability-bridge.md) may generate the track-field portion of this codec from the governed field descriptor, but this RFC owns its durable compatibility requirements.

### One codec and persistence boundary

Introduce a presentation-state codec that owns:

- version detection;
- legacy and current decoding;
- semantic validation;
- conversion between stable ids and typed runtime values;
- canonical version-1 encoding;
- structured diagnostics and fallback decisions.

GTK widgets and workspace-session code exchange typed `TrackListViewConfig`, column-layout, and preset values with this boundary.
They do not cast serialized integers directly to enums or duplicate migration policy.

The codec is independent of GTK so CLI tests and future frontends can validate the same persisted vocabulary.
The storage adapter remains responsible for I/O errors and the current [atomic file replacement contract](../spec/persistence/atomic-replacement.md).

### Frozen version-0 migration

Absence of a version denotes the existing numeric version 0 only for stores known to have used that format.
Version-0 numeric values decode through frozen tables that record the exact historical meaning of every accepted ordinal.

Migration must not use `static_cast<CurrentEnum>(legacyNumber)`.
The frozen table remains unchanged when current enum declarations change.
Removed historical values map through an explicit replacement or make that containing object invalid.

After successful decoding, version-0 data becomes the same typed intermediate representation as version 1.
The next successful save writes canonical version 1; readers do not rewrite files merely as a side effect of inspection.

An absent version in a new or unrelated store is not assumed to be version 0.
The storage owner supplies the expected document kind so random configuration cannot be interpreted as a legacy layout.

### Validation and fail-closed recovery

Decoding is transactional per independently meaningful object:

1. Parse the complete object into an untrusted representation.
2. Resolve every stable id or legacy value.
3. Validate required fields, duplicate columns, widths, sort direction, group/sort compatibility, preset identity, and any existing structural invariants.
4. Install the typed object only when all required checks pass.

An invalid column layout falls back to the documented default layout; it is not partially applied with the invalid columns removed.
An invalid open-view presentation falls back to its default preset while other valid open views remain recoverable.
An invalid custom preset is skipped as one object with a diagnostic; its id is not rebound to partially decoded content.

Unknown future ids and unsupported future schema versions produce structured diagnostics and preserve the original stored bytes until an explicit successful save.
The application must not overwrite a newer unsupported document with defaults during startup.

### Canonical and atomic writes

Writers emit only the latest supported version with stable ids, deterministic field order where the encoding exposes order, and no legacy numeric aliases.
The storage adapter writes a complete document according to the current [atomic file replacement contract](../spec/persistence/atomic-replacement.md).

Migration and ordinary edits share the same encoder.
If persistence fails, the last durable document remains intact and the in-memory state reports the save failure through its existing owner.

### Compatibility ownership

The durable format reference records:

- current schema version;
- every stable id and its semantics;
- required and optional fields;
- validation and fallback rules;
- version-0 frozen ordinal mappings;
- supported migration paths;
- unknown-version behavior.

Architecture documentation continues to explain why presentation state crosses frontend, UI-model, runtime, and persistence boundaries.
Specifications own current behavior after implementation; this RFC owns only the proposal until then.

## Alternatives

### Keep numeric enums and promise never to reorder them

This makes an accidental source edit a persistence break and provides no readable or namespace-safe contract.
Deprecated ordinal slots also accumulate permanently in implementation enums.

### Serialize C++ enum names

Names are readable but still couple source refactoring to the file format.
Explicit ids let implementation names change without migration.

### Add only a version number

A version permits migration but does not prevent every ordinary enum insertion from requiring one.
Stable ids reduce migration frequency and make files diagnosable.

### Silently drop unknown fields

Partial recovery appears resilient but can change column, sort, and group semantics in surprising ways.
Transactional fallback at the object boundary is predictable and testable.

### Maintain separate GTK and workspace codecs

That preserves local ownership but duplicates stable-id and migration tables for the same model types.
One model codec with separate storage adapters prevents drift without merging the stores.

### Rewrite legacy state immediately on load

Eager rewriting turns a read into a mutation, can destroy unsupported future data after downgrade, and complicates startup failure handling.
Canonical write-on-next-save is safer.

## Compatibility and migration

Implementation phases are:

1. Inventory every persisted presentation field and capture current numeric behavior in immutable version-0 golden fixtures.
2. Define stable-id codecs and the version-1 format reference without changing current writes.
3. Add a side-effect-free decoder for version 0 and version 1, with structured errors and object-level fallback.
4. Route GTK column layouts and workspace presentation state through the shared codec.
5. Switch writes to canonical version 1 and atomic replacement.
6. Remove direct enum casts and duplicate serializers after differential tests cover all historical values.

No user action is required for a valid version-0 document.
It remains readable and is upgraded on the next explicit save.
Unsupported future versions remain untouched and produce a visible diagnostic rather than being downgraded.

## Validation

- Golden fixtures map every accepted version-0 numeric field, sort, group, and direction to its historical semantic value.
- Reordering current enum declarations does not change version-0 decoding or version-1 output.
- Every current typed value round-trips through its stable id and canonical version-1 document.
- Unknown ids, malformed ids, duplicates, invalid combinations, and unsupported versions exercise the documented object-level fallback.
- One invalid custom preset does not discard valid presets or unrelated open views.
- A corrupt layout never installs a partially valid column set.
- Reading version 0, invalid data, or an unsupported future version performs no write.
- The first successful save after version-0 load emits version 1 with no numeric enum values.
- Simulated write failure preserves the last durable document.
- GTK and workspace adapters produce identical typed values for shared presentation fields.
- Completed implementation passes `./ao check`.

## Open questions

- Should GTK column layout and workspace session remain separate versioned documents or become sections of one envelope while still sharing the codec?
- Which current presentation objects are truly independent fallback boundaries: view, layout, preset, or workspace?
- Should unsupported future documents surface only a diagnostic, or also offer an explicit user-authorized reset/export action?
- Which component is the long-term owner of stable track-field ids if RFC 0008 is not implemented first?
- Are the current platform replacement and crash-durability limits sufficient for presentation-state migration, should [RFC 0014](0014-observable-atomic-replacement.md) be required for explicit acknowledgement, or does this payload need a stronger recovery protocol?

## Promotion plan

If accepted and implemented, update:

- [Presentation architecture](../architecture/presentation.md) with the codec, persistence boundary, and recovery ownership;
- [Workspace architecture](../architecture/workspace.md) if version gates change semantic workspace restoration, and [interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) if recovery changes startup ordering;
- [Persistence and managed-state architecture](../architecture/persistence-and-managed-state.md) with version detection, atomic writes, unsupported-version preservation, and the implemented schema-owner, codec, and restore-commit boundaries;
- [Track presentation](../spec/presentation/track-presentation.md) and list-preference specifications with fallback and migration behavior;
- [Workspace session specification](../spec/workspace/session.md) with version detection, candidate rejection, fallback, and migration behavior;
- [Workspace session state reference](../reference/workspace/session-state.md) with the implemented versioned envelope, stable field ids, and frozen legacy mappings;
- [Track field reference](../reference/library/model/track-field.md) with stable persisted field ids;
- a new presentation-state format reference containing the complete schemas and frozen version-0 mappings;
- frontend and workspace development guidance if operators need diagnostics or reset/export procedures.
