---
id: rfc.0008.track-capability-bridge
type: rfc
status: draft
domain: query
summary: Proposes a typed, exhaustively validated bridge between core query variables and runtime track capabilities.
depends-on: none
---
# RFC 0008: Declarative track capability bridge

## Problem

Aobus deliberately has different field models for different authorities:

- core `query::Field` and its variable catalog own expression evaluation;
- runtime `TrackFieldDefinition` owns application ids, editing, presentation, sorting, grouping, completion, and filter bridges;
- presentation recommendation interprets expression variables as view-shape signals.

Keeping those types separate is correct, but their current bridges are duplicated and stringly typed.
`TrackFieldDefinition::filterExpressionVariable` stores source text, `QueryExpressionCompleter` contains a manual `query::Field -> TrackField` switch, quick-search expansion hard-codes a field list, and `TrackPresentationRecommender` compares raw variable names in a visitor.

There is no exhaustive cross-catalog test.
The current `Codec` field demonstrates drift: the predicate language supports `@codec`, but the runtime catalog does not expose a `Codec -> @codec` filter bridge.
New query or track fields can silently compile and display while being absent from value completion, quick search, generic filter actions, or recommendation.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0007](0007-revisioned-completion-vocabulary.md) must consume the same declared value-completion capability set when both proposals are implemented.

## Goals

- Preserve distinct core query and runtime presentation/editing field types.
- Replace source-text field bridges with typed identities.
- Make every query/runtime mapping exhaustive or explicitly absent with a reason.
- Derive reverse lookup, quick-search participation, completion eligibility, and recommendation signals from declarative capability data.
- Eliminate manual switches and raw variable-name comparisons at integration seams.
- Make the exact reference surface mechanically checkable against code.
- Add `@codec` bridging or explicitly reject it through a reviewed capability decision.

## Non-goals

- Creating one universal field enum across core, runtime, storage, YAML, and UI.
- Moving presentation policy into `ao_query`.
- Exposing runtime labels, widths, or editability to the core query library.
- Automatically making every presentable field queryable, sortable, or groupable.
- Changing predicate syntax or adding new stored metadata.
- Replacing completion indexing; [RFC 0007](0007-revisioned-completion-vocabulary.md) owns vocabulary maintenance.

## Proposed design

### Typed query identity

Promote a stable public query-variable descriptor from the existing core catalog:

```cpp
struct QueryVariableId final
{
  query::VariableType type;
  query::Field field;
};

struct QueryVariableDescriptor final
{
  QueryVariableId id;
  std::string_view canonicalName;
  std::span<std::string_view const> aliases;
  QueryValueKind valueKind;
};
```

The descriptor remains owned by `ao_query` and contains no runtime or presentation type.
Parser field resolution, completion, CLI help, diagnostics, and serialization continue to use this one catalog.

### Runtime capability bridge

Replace `TrackFieldDefinition::filterExpressionVariable` with typed optional capabilities:

```cpp
enum class QuickSearchRole
{
  None,
  Text,
  DynamicTag,
};

enum class PresentationSignal
{
  None,
  Album,
  Artist,
  AlbumArtist,
  Genre,
  Year,
  Composer,
  Work,
  Technical,
  Tag,
};

struct TrackFieldQueryCapabilities final
{
  std::optional<query::QueryVariableId> variable;
  QuickSearchRole quickSearchRole;
  PresentationSignal presentationSignal;
  bool valueCompletion;
};
```

The concrete layout may embed these fields into `TrackFieldDefinition`, but the meanings remain explicit.
Canonical expression text is obtained from the core descriptor/serializer rather than stored as another string.

Dynamic tags remain a domain-level capability because one `TrackField::Tags` represents many `#name` variables.
Custom metadata remains dynamic and does not require one runtime field per key.

### Generated reverse bridge

Runtime builds an indexed reverse mapping from `query::Field` to zero or one application `TrackField` where scalar field completion or generic field UI needs it.
Duplicate typed mappings are a compile-time or startup contract failure.

`QueryExpressionCompleter` uses this mapping instead of a switch.
Only mappings whose runtime definition enables value completion request a live vocabulary.

The bridge explicitly classifies query fields with no runtime field counterpart, such as cover-art predicate identity or dynamic custom/tag fields.
An omitted mapping is therefore reviewed data, not forgotten code.

### Quick-search derivation

UIModel quick-search expansion iterates runtime definitions with `QuickSearchRole::Text` in declared catalog order and builds AST predicates through their typed query variables.
It appends the dynamic tag predicate through the tag-domain capability.

The existing search field set remains the initial behavior unless acceptance deliberately changes it.
Conductor, ensemble, movement, soloist, technical fields, and codec are evaluated individually for search usefulness rather than becoming included merely because they are queryable.

### Presentation recommendation derivation

Core query analysis exposes referenced typed fields from a parsed AST without resolving presentation policy:

```cpp
std::vector<QueryVariableId> referencedQueryVariables(Expression const& expression);
```

UIModel maps those ids through runtime capability data and applies the existing `PresentationSignal` priority.
It no longer compares canonical source names such as `"work"` or `"sampleRate"`.

Recommendation remains Presentation-owned and may group several query variables under one signal.

### Exhaustive governance

Tests enforce:

- every core `$`/`@` descriptor resolves to its declared `query::Field`;
- every runtime typed bridge resolves back to the same canonical descriptor;
- no two runtime fields claim the same scalar reverse bridge unless explicitly supported;
- every value-completion capability is accepted by RFC 0007's index field set;
- every quick-search field has a scalar string predicate compatible with `~`;
- every presentation signal maps to a known recommendation branch;
- every query field is either bridged or listed in an explicit core-only/dynamic exception table.

The documentation check may consume a generated catalog artifact or a unit-test-locked table.
Hand-written reference remains acceptable only while tests exhaustively compare every row.

## Alternatives

### Merge `query::Field` and `TrackField`

This creates a dependency from core query mechanics toward application presentation concepts and forces synthetic fields into query semantics.
The selected bridge preserves authority boundaries.

### Keep source-text mappings and add tests

Tests reduce drift but every consumer still reparses or compares strings and manual reverse lookup remains duplicated.
Typed identity removes an entire class of spelling errors.

### Generate all catalogs from one schema file

A single schema can be effective, but it risks making one file the owner of unrelated core and UI policy.
The selected design first defines typed ownership and exhaustive bridges; generation can be added later.

### Let each consumer choose fields independently

Independent lists allow local optimization but have already produced silent capability gaps.
Policy choices remain local through capability flags, while identity and exhaustiveness become shared.

## Compatibility and migration

No library or configuration schema change is required merely to type the bridge.
Canonical query variable names and runtime field ids remain unchanged.

Implementation phases are:

1. Add public typed query descriptors and referenced-variable analysis without changing current consumers.
2. Add typed runtime capabilities beside existing string mappings and exhaustive agreement tests.
3. Migrate query value completion, quick-search expansion, and presentation recommendation.
4. Decide and implement the `Codec -> @codec` bridge and any intentionally omitted quick-search roles.
5. Remove string mappings, manual switches, and raw variable-name visitors.
6. Add generated/reference drift validation if maintenance evidence justifies it.

During migration, the typed and string bridge must agree in tests; no production consumer may add a third mapping.

## Validation

- Exhaustive tests fail when a new query or runtime field lacks a mapping or explicit exception.
- Every existing canonical variable and alias continues to parse, complete, and appear in CLI help.
- Query value completion offers the same fields and values before and after migration.
- Quick-search expansion produces predicate-equivalent canonical AST output for the current field set.
- Presentation recommendation produces the same preset for every existing signal and priority combination.
- `Codec` generic filter affordance follows the reviewed bridge decision and has focused tests.
- Dynamic tags and custom keys remain supported without fake `TrackField` instances.
- Core `ao_query` has no dependency on runtime/UIModel headers.
- Runtime/UIModel boundary guardrails continue to pass.
- Reference tables agree exhaustively with the implemented descriptors.
- Completed implementation passes `./ao check`.

## Open questions

- Should `QueryVariableId` expose the internal bytecode `query::Field`, or should it use a separate stable semantic enum translated during compilation?
- Should quick-search field order remain runtime catalog order or use an explicit ranked sequence?
- Is codec valuable enough for generic field-to-filter actions even though it is not a free-text value-completion field?
- Should presentation signals live directly in runtime field definitions or in a separate UIModel table keyed by typed query identity?

## Promotion plan

If accepted and implemented, update:

- [Track expression architecture](../architecture/track-expression.md) with typed catalog ownership;
- [Presentation architecture](../architecture/presentation.md) with the typed recommendation seam;
- [Predicate language](../reference/query/predicate-language.md) if descriptor exposure changes its authority map;
- [Runtime track field catalog](../reference/library/model/track-field.md) with typed bridges and capability results;
- [Query expression completion](../spec/query/expression-completion.md), [track filtering](../spec/presentation/track-filter.md), and [track-list presentation](../spec/presentation/track-presentation.md) with derived consumer behavior;
- development guidance if catalog generation or review rules become repository policy.
