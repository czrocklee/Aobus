---
id: rfc.0024.versioned-predicate-dialect
type: rfc
status: rejected
domain: query
summary: Rejects a per-expression predicate dialect and assigns retained-text compatibility to each containing format or protocol version.
depends-on: none
---
# RFC 0024: Versioned predicate dialect

## Disposition

Rejected on 2026-07-15.

The semantic-drift risk is real, but the proposed per-expression dialect id, versioned compiler registry, mixed-version evaluation, and unknown-dialect round trip are not justified by a current product requirement.
Aobus has one predicate implementation, no retained expression that must select between two supported meanings, and no demonstrated need to keep multiple predicate versions live in one library.

The proposal would nevertheless add language identity to Smart List records, library YAML, workspace and playback state, CLI input, completion, diagnostics, and most query APIs.
It would also require old parser/catalog/evaluator behavior to remain executable without a concrete incompatible language change to migrate.

The adopted policy uses the compatibility version already owned by each containing surface:

- Smart List filter text is part of the host-local library database contract governed by `ao::library::kLibraryVersion`.
- Library YAML `filter` text is part of the root YAML format version.
- Playback quick-filter text is part of the playback state's `schemaVersion` contract.
- Workspace filter text is not governed by the current presentation-only version marker; RFC 0017 owns the proposed complete workspace schema boundary.
- Predicate text accepted by CLI automation is command behavior governed by the CLI contract and, if RFC 0029 is implemented, by the selected protocol major.

No retained expression carries a separate dialect id or version.
The current authorities are the [track expression architecture](../architecture/track-expression.md), [predicate evaluation specification](../spec/query/predicate-evaluation.md), [predicate language reference](../reference/query/predicate-language.md), and the exact references for each containing format.
They supersede this proposal; this RFC remains as the record of the rejected larger design.

## Problem

Predicate expressions are retained as text and reparsed against the installed parser, field catalog, unit rules, dictionary binding, and evaluator semantics.
The following changes can therefore reject or silently reinterpret retained data:

- changing operator precedence, tokenization, escaping, aliases, or accepted literals;
- renaming or removing a field, unit, codec constant, or shorthand;
- changing missing-value, comparison, list, range, or dictionary truth semantics; and
- fixing a parser or evaluator defect on which retained text relied.

Syntax rejection is visible, while semantic drift can leave a Smart List valid but change which tracks it selects.
That compatibility risk needs an explicit owner even though the durable value remains plain text and AST nodes, opcodes, access profiles, and execution plans remain transient.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0006](0006-coherent-derived-track-views.md), [RFC 0008](0008-declarative-track-capability-bridge.md), [RFC 0009](0009-pure-expression-binding.md).

The rejected proposal would have carried a dialect-bound value through RFC 0006, versioned the RFC 0008 catalog by dialect, and selected that dialect during RFC 0009 binding.
The adopted container-version policy requires none of those integrations.

## Goals

The proposal sought to:

- give retained and automation-relevant predicate text an explicit language identity;
- freeze the current grammar, catalog, compilation, and truth behavior as an initial version;
- preserve text while keeping AST and execution artifacts transient;
- dispatch old and new predicate meanings through one compiler boundary; and
- migrate or preserve unsupported expression versions without silent reinterpretation.

The compatibility-owner and transient-artifact goals are retained.
Per-expression identity, mixed-version dispatch, and unknown-dialect preservation are deliberately not adopted.

## Non-goals

- Persist AST nodes, execution plans, dictionary ids, or access-profile opcodes.
- Change current predicate syntax or truth semantics.
- Treat the application build version as a persistence schema.
- Version scalar format expressions through this RFC.
- Move Smart List membership or view lifecycle into the query library.

## Rejected design

The rejected design introduced a value such as:

```text
PredicateText {
  dialect: PredicateDialectId
  version: PredicateDialectVersion
  text: UTF-8 string
}
```

Every retention and automation boundary would carry that complete value.
A central registry would select a versioned grammar, field/alias/unit catalog, semantic compiler, evaluator rules, diagnostics, and optional migration serializer.
Existing unversioned text would acquire an initial identity, unsupported future identities would fail closed or round-trip opaquely, and multiple versions could remain executable in one process and library.

This solves hypothetical mixed-version coexistence, but that is not a current Aobus requirement.
It multiplies serialization fields, API types, compatibility fixtures, completion catalogs, migration states, and retained old semantics before there is one concrete incompatible predicate change to drive their shape.

## Adopted compatibility policy

### Containing surface owns the contract

| Retained or automated text | Compatibility owner |
|---|---|
| Smart List `filter` in LMDB | Library database version, `kLibraryVersion` |
| Smart List `filter` in library YAML | YAML root `version` |
| Playback `quickFilterExpression` | Playback `schemaVersion` |
| Workspace `filterExpression` | Workspace schema policy; currently outside the `presentationVersion` compatibility boundary |
| CLI predicate input | CLI command/protocol compatibility policy |

The owner covers more than byte layout.
For predicate text, its contract includes accepted grammar, field and alias catalog, literal and unit meanings, binding behavior, and evaluation truth rules.

A change is incompatible when it permits retained text that an existing same-version consumer cannot handle, or when it can alter whether previously retained text parses or compiles, what it binds to, or which tracks it matches.
Such a change must either:

1. increment the containing version and follow that surface's rejection or migration policy; or
2. provide an explicitly tested backward-compatible implementation that preserves the old observable result.

Adding an optimization or refactoring parser/compiler internals does not require a version change when accepted text and observable results remain unchanged.

### Library behavior

Physical library format version `4` includes the current Smart List predicate contract even though list records store only filter bytes.
The current implementation accepts only an exact `kLibraryVersion` match and has no in-place migration path; a mismatch returns `CorruptData`, and recovery requires resetting and rescanning the host-local index.

If a future incompatible predicate change needs to preserve Smart Lists rather than rebuild them, that concrete change must introduce a library-format migration.
The target format still increments `kLibraryVersion`; the migration replaces reset-and-rescan recovery, not the version boundary.
It must interpret old filters under the old contract, rewrite or validate them for the new contract, commit list changes atomically, and publish the new metadata version only after the converted data is valid.
No generic dialect registry is introduced in advance.

### Other retained surfaces

The library YAML and playback-session versions independently govern predicate text they contain; neither derives its version from `kLibraryVersion`.
Their version must change when a predicate change would reinterpret retained text; an importer may then provide an explicit tested path from the old version instead of rejecting it.

The current workspace payload versions only nested presentation vocabulary and has no root version or migration layer governing filter text, so it cannot make the same compatibility promise.
That is an existing workspace-schema limitation rather than a reason to put a language version inside every expression.

## Alternatives

### Promise that the language never changes

Rejected because necessary corrections may be incompatible and accidental semantic drift still needs a review gate.
The container version and compatibility tests supply that gate without a second version axis.

### Treat the application version as the language version

Rejected because application releases and persisted formats evolve at different rates, and one application can read several independently governed surfaces.

### Persist AST or bytecode

Rejected because internal shapes would couple durable data to compiler layout, opcode evolution, and implementation details.
Text remains the durable representation.

## Compatibility and migration

Rejecting this proposal changes no serialized field and requires no data migration.
Existing expressions remain plain text and continue to use the current parser/compiler/evaluator.

Future incompatible changes are reviewed against every containing surface that retains or promises automation behavior for predicate text.
The change supplies the specific version bump, adapter, migration, and regression fixtures needed by those affected surfaces rather than activating a permanent multi-dialect subsystem.

## Validation

Current grammar, compilation, and truth behavior remain protected by query tests.
Smart List persistence and exact version rejection remain protected by library layout and `MusicLibrary` tests.
Library YAML, playback, workspace, and CLI tests own their containing-surface behavior.

Any future incompatible predicate change must add fixtures proving the selected behavior for old retained text: rejection before reinterpretation, a version-specific adapter, or an atomic migration.
A full `./ao check` remains the implementation gate for such a change.

## Open questions

None.
A concrete requirement for simultaneous predicate meanings or lossless unknown-language round trip would require a new evidence-backed RFC rather than reopening this design implicitly.

## Promotion plan

No proposal promotion remains.
The adopted policy is current in:

- [Track expression architecture](../architecture/track-expression.md)
- [Predicate evaluation specification](../spec/query/predicate-evaluation.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Library database reference](../reference/library/storage/database.md)
- [Library YAML format](../reference/library/format/yaml.md)
- [Playback session state](../reference/playback/session-state.md)
- [Workspace session state](../reference/workspace/session-state.md)
