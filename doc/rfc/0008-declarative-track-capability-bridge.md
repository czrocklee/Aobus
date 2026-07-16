---
id: rfc.0008.track-capability-bridge
type: rfc
status: implemented
domain: query
summary: Implements a public typed query-variable catalog and an exhaustive runtime field bridge while retaining quick-search and presentation policy in UIModel.
depends-on: none
---
# RFC 0008: Declarative track capability bridge

## Disposition

Implemented on 2026-07-16 with a narrower typed bridge than originally proposed.

The implementation:

- promotes `QueryVariableDescriptor` as the public core catalog entry for variable type, query field, canonical name, and aliases;
- replaces `TrackFieldDefinition::filterExpressionVariable` with an optional typed `query::Field`;
- provides typed forward and reverse runtime lookup and derives canonical expression text from the core catalog;
- routes runtime completion and CLI help through the core catalog and variable formatter, derives field diagnostics from the catalog, and serializes quick-filter constants instead of reconstructing source spellings;
- derives completion eligibility from the runtime field definition and dictionary extraction from the associated query field;
- lets quick search retain its explicit UIModel-owned field order while representing that policy as typed `TrackField` values shared by resolution and completion;
- resolves presentation variables, including aliases, through the core descriptor before applying UIModel-owned recommendation policy;
- adds the previously missing `TrackField::Codec -> query::Field::Codec` bridge; and
- exhaustively tests catalog resolution, bridge uniqueness, supported value completion, and the explicit core-only `CoverArtId` exception.

The larger `QueryVariableId` wrapper, runtime `QuickSearchRole` and `PresentationSignal` flags, referenced-variable analysis API, and generated catalog artifact were not needed.
They would make runtime field data own UI policy or add types without removing another representation.

The [track expression architecture](../architecture/track-expression.md), [predicate language reference](../reference/query/predicate-language.md), [runtime track field catalog](../reference/library/model/track-field.md), and presentation/query specifications linked in the promotion section own current behavior.
Those authorities supersede this proposal; this RFC records the chosen boundary and the broader mechanisms that were not adopted.

## Problem

Aobus deliberately has different field models for different authorities:

- core `query::Field` and its variable catalog own expression evaluation;
- runtime `TrackFieldDefinition` owns application ids, editing, presentation, sorting, grouping, completion, and filter bridges; and
- presentation recommendation interprets expression variables as view-shape signals.

Keeping those types separate is correct, but their bridges were duplicated and stringly typed.
`TrackFieldDefinition::filterExpressionVariable` stored source text, `QueryExpressionCompleter` contained a manual `query::Field -> TrackField` switch, quick-search expansion hard-coded source variables, and `TrackPresentationRecommender` compared raw variable names in a visitor.

There was no exhaustive cross-catalog test.
The `Codec` field demonstrated the resulting drift: the predicate language supported `@codec`, but the runtime catalog did not expose the corresponding filter bridge.
New query or track fields could silently compile and display while being absent from value completion, quick search, generic filter actions, or recommendation.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0007](0007-revisioned-completion-vocabulary.md) shares the declared value-completion capability set.

RFC 0007's larger index was rejected, but its implemented deletion-correct cache now consumes the bridge established here.

## Goals

- Preserve distinct core query and runtime presentation/editing field types.
- Replace source-text field bridges with typed identities.
- Make every query/runtime mapping exhaustive or explicitly absent with a reason.
- Eliminate manual field-mapping switches and raw variable-name comparisons at integration seams.
- Make the exact reference surface mechanically checkable against code.
- Add the missing `@codec` bridge.

The original proposal also sought to derive quick-search and presentation policy from runtime capability flags.
The implementation deliberately leaves those choices in UIModel and shares only typed identity.

## Non-goals

- Creating one universal field enum across core, runtime, storage, YAML, and UI.
- Moving presentation or quick-search policy into `ao_query` or runtime field metadata.
- Exposing runtime labels, widths, or editability to the core query library.
- Automatically making every presentable field queryable, sortable, groupable, searchable, or value-completable.
- Changing predicate syntax or adding stored metadata.
- Replacing completion indexing; [RFC 0007](0007-revisioned-completion-vocabulary.md) owns that disposition.

## Implemented design

### Public query-variable descriptors

The core catalog exposes one public descriptor type:

```cpp
struct QueryVariableDescriptor final
{
  VariableType type;
  Field field;
  std::string_view canonicalName;
  std::span<std::string_view const> aliases;
};
```

Callers enumerate descriptors by variable type or resolve them by type/name and by query field.
Field parsing, query completion, CLI help, and canonical field diagnostics consume the same catalog.
Aliases remain query-language data rather than being copied into runtime or UIModel tables.

### Runtime typed bridge

`TrackFieldDefinition` carries `std::optional<query::Field> optQueryField`.
The absence of a value is intentional for runtime-only fields such as file path and synthetic presentation fields.

Runtime exposes typed lookup in both directions.
`trackFieldFilterExpressionVariable()` is retained as a source-text convenience for UI consumers, but formats the query descriptor through the core helper rather than storing or assembling another spelling.

Every metadata/property query descriptor maps to at most one runtime field.
`query::Field::CoverArtId` is the explicit core-only exception because there is no corresponding scalar runtime `TrackField`.
Dynamic tag and custom variables remain domain variables and do not require synthetic field instances.

### Completion composition

`QueryExpressionCompleter` resolves a query field through the typed reverse bridge before requesting value completion.
It no longer owns a manual mapping switch.

`CompletionService` includes every dictionary-backed `TrackFieldDefinition` in one source-preserving frequency snapshot and exposes individual values only when the definition's `valueCompletion` flag permits it.
The associated query field determines dictionary-id extraction through core query helpers.
This removes the service's private completion-field list and extraction switch while keeping runtime ownership of which presentable fields receive live vocabularies.

The service's aggregate vocabulary API accepts a caller-owned typed field set rather than reading a Quick-search flag from the runtime catalog.
It validates title/dictionary-backed identities and materializes their combined frequencies from the same shared snapshot; UIModel remains responsible for selecting the set and consuming its unordered results.

### UIModel-owned policy

Quick search keeps an explicit ordered list of `TrackField` values because field selection and ranking are product policy, not intrinsic query capabilities.
It obtains canonical source variables through the runtime bridge, so the list contains typed identities rather than duplicated strings.
Term constants and tag variables use the core serializer, including terms containing both quote styles.
The shared `TrackFilterCompleter` reuses the same field list and expression boundary for GTK and TUI, combines those fields and tags into one live vocabulary request, and delegates explicit structured input to `QueryExpressionCompleter`.

Presentation recommendation likewise remains UIModel policy.
The AST visitor resolves canonical names and aliases through `QueryVariableDescriptor`, then switches on typed `query::Field` values to apply existing recommendation priority.
No presentation signal is added to the runtime catalog.

### Exhaustive governance

Tests enforce:

- every core metadata/property descriptor resolves by canonical name, alias, and query field;
- every runtime query bridge resolves back to one core descriptor;
- no two runtime fields claim the same query field;
- every metadata/property query field is bridged except the explicit `CoverArtId` exception;
- every value-completion field has a dictionary-backed query field;
- quick-search output retains its exact ordered predicate surface; and
- presentation aliases produce the same recommendation as their canonical names.

Reference documents list the public catalog and runtime bridge as implementation authorities rather than maintaining another hand-written mapping table.

## Alternatives

### Merge `query::Field` and `TrackField`

This would create a dependency from core query mechanics toward application presentation concepts and force runtime-only fields into query semantics.
The implemented bridge preserves authority boundaries.

### Keep source-text mappings and add tests

Tests would reduce drift, but consumers would still parse or compare spellings and manual reverse lookup would remain duplicated.
Typed identity removes that representation.

### Put quick-search and presentation flags in `TrackFieldDefinition`

That centralizes more behavior, but makes a runtime identity catalog own UIModel selection, order, and recommendation policy.
Small typed UIModel tables and switches state those choices more directly.

### Generate all catalogs from one schema file

A single schema could be useful if drift recurs, but it would risk making one file the owner of unrelated core and UI policy.
The implemented exhaustive contracts provide evidence before introducing generation machinery.

## Compatibility and migration

No library data, configuration schema, runtime field id, or predicate syntax changed.
Canonical variable names remain stable.

The old stored source-text member, completion summary representation, private completion field list, and manual query-to-runtime switches were removed in one migration.
`Codec` now supports the generic `@codec` filter bridge.
Presentation recommendation now treats aliases such as `$w` and `@sr` identically to their canonical forms, correcting previously string-dependent behavior.
GTK and TUI Quick filters now share one UIModel completion policy without adding Quick-search roles to runtime field definitions.

## Validation

The implementation is validated by:

- exhaustive core descriptor and runtime bridge unit tests;
- focused query completion, runtime completion, quick-search, and presentation recommendation regressions;
- aggregate vocabulary and shared GTK/TUI Quick-filter completion regressions;
- insertion, deletion, and reset invalidation coverage shared with RFC 0007;
- documentation validation; and
- the repository full validation gate.

## Open questions

None for this RFC.
New quick-search fields or presentation signals remain ordinary reviewed UIModel policy changes.
Catalog generation should be reconsidered only if the exhaustive typed contracts prove insufficient in practice.

## Promotion plan

No proposal promotion remains.
Current behavior is owned by:

- [Track expression architecture](../architecture/track-expression.md)
- [Predicate language](../reference/query/predicate-language.md)
- [Runtime track field catalog](../reference/library/model/track-field.md)
- [Query expression completion](../spec/query/expression-completion.md)
- [Track-field value completion](../spec/presentation/field-completion.md)
- [Track filtering](../spec/presentation/track-filter.md)
- [Track-list presentation](../spec/presentation/track-presentation.md)
