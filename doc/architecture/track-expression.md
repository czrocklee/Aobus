---
id: architecture.track-expression
type: architecture
status: current
domain: query
summary: Defines expression ownership from authoring through predicate membership, completion, and per-track string evaluation.
---
# Track expression architecture

## Scope

This document owns the structural boundary of Aobus track expressions.
It explains how shared syntax becomes a predicate, a completion context, or a per-track string and how those results compose with library sources and presentation.

It does not define the exact expression grammar, predicate truth rules, completion ranking, format output, saved-List membership behavior, saved ordering, or track-list presentation behavior.
Those contracts belong to the linked specifications and references.

## System context

Track expressions cross the layers from the [system architecture](system-overview.md) without collapsing their responsibilities:

```text
GTK / TUI input
  -> UIModel authoring policy
  -> runtime view, completion, or library command
  -> ao::query parser/compiler/evaluator
  -> ao::library::TrackView

CLI filter / format input
  -> ao::query parser/compiler/evaluator
  -> ao::library::TrackView
```

The core `ao_query` library is public under `include/ao/query/` and implemented under `lib/query/`.
It depends on `ao_library` for track views and dictionary resolution but does not depend on runtime, UIModel, or a frontend.

Application composition lives in `app/runtime/`, platform-neutral input and recommendation policy lives in `app/uimodel/`, and GTK, TUI, and CLI remain adapters.

## Responsibilities

### Shared expression core

`ao::query` owns the parser, AST, serializer, public typed variable-descriptor catalog, field resolution, tolerant completion analysis, predicate compiler and evaluator, and format compiler and evaluator.
The parser accepts the common syntactic superset while the predicate and format compilers enforce distinct result types.

An `ExecutionPlan` answers whether one track matches.
A `FormatPlan` produces one string from one track.
Neither plan owns source membership, presentation shape, or frontend state.
Compilation is observationally pure: both plan types own their dictionary symbol text and retain no library or dictionary pointer.
Evaluation that needs dictionary data receives an explicit bounded read context and a plan-specific binding.
A Unicode-caseless text-substring predicate stores its folded literal in the plan and derives field keys through binding-local bounded caches; neither the plan nor the library persists derived Unicode keys.

### Library and runtime consumers

`LibraryCommands` validates a saved-List expression before committing its text as part of a List definition.
`TrackSourceCache`, `SmartListSource`, and `SmartListEvaluator` compile that text and maintain the resulting ordered membership over an upstream source.
`ListOrderSource` then applies the List's independent saved-rank overlay; expression evaluation neither reads nor mutates that rank.
The same source machinery materializes transient `ViewService` filters without persisting a new list.
During one membership rebuild, `SmartListEvaluator` creates one dictionary read cache/context, binds each plan once, and reuses those bindings and any caseless-key caches across track evaluations; cache presence and eviction do not change predicate results.

The runtime `TrackFieldDefinition` catalog bridges application fields to those core descriptors with typed `query::Field` identities and carries no authored field labels.
`CompletionService` derives its value-completable fields and dictionary extraction from that bridge, and `QueryExpressionCompleter` combines the live vocabularies with core completion analysis through the reverse mapping.
The service captures titles, tags, custom keys, and dictionary-backed track fields in one source-preserving live frequency snapshot after invalidation.
Single-field completion and a caller-selected cross-field aggregate materialize from that snapshot without additional track-store scans; runtime owns storage access and invalidation but not the aggregate field choice.
Interactive composition roots may also inject an ICU-free completion-alias policy.
The runtime derives transient romanized aliases once per dictionary id or inline-title slot in that same snapshot, while consumers continue to display and insert the admitted source text.
The ICU implementation remains in an interactive leaf target; the CLI supplies no policy and neither runtime headers nor the query core expose ICU types.
The runtime does not redefine expression grammar or field aliases.

### UIModel authoring and recommendation

UIModel owns platform-neutral interpretation of quick-filter input, filter view state, List draft and preview policy, and track-presentation recommendation.
It may parse or serialize a core expression, but it cannot implement a second grammar or evaluator.

Quick-search policy retains a typed runtime-field list, obtains canonical expression variables through the bridge, emits the core language's ordinary substring operator, and delegates system-variable source text to the core variable formatter and constants and dynamic variables to the core serializer.
`TrackFilterCompleter` reuses that field list and the resolver's explicit-expression boundary, ranks matching aggregate values, and otherwise delegates structured input to the runtime query completer.
Completion items retain typed field/alias/operator/logical-operator roles or frequency arguments until UIModel's presentation catalog resolves their secondary text.
Presentation recommendation resolves parsed names and aliases to typed query fields before applying its UIModel-local signal priority.
That inspection selects a view shape only; it never changes membership or expression meaning.

### Frontend adapters

GTK and TUI bind the same UIModel filter completion and resolution results to their native interaction models.
WinUI binds the same resolution policy but does not currently expose filter completion candidates.
All three send the resolved expression to runtime rather than evaluating tracks themselves.

The CLI uses the core predicate path for `--filter` and the core string path for `track show --format`.
The format path currently terminates at plain CLI output and is not a track-list presentation mechanism.

## Boundaries and dependency direction

The content and shape axes of a runtime track-list view remain independent:

```text
listId + filterExpression
  -> TrackSource membership

saved List orderTrackIds
  -> source order before presentation

TrackPresentationSpec
  -> sort + group + visible fields

TrackSource + TrackPresentationSpec
  -> TrackListProjection
  -> rows and sections
```

- Expressions may determine membership or a scalar output, but never grouping, sort order, visible columns, or frontend layout.
- Presentation may inspect expression variables for a recommendation, but never owns syntax, compilation, or predicate truth.
- Projections consume a source plus a presentation spec; they do not parse user expressions.
- Core query code may read core track values and dictionaries but cannot depend on runtime field, completion, view, UIModel, or frontend types.
- Runtime and UIModel may depend on the core query API; the dependency never points back from `ao_query`.
- Exact query variables belong to the query language reference.
  The runtime track-field reference owns only the application-field capability catalog and its explicit mapping to those variables.

## Data and control flow

### Persisted List

```text
frontend editor or CLI command
  -> UIModel draft when interactive
  -> LibraryCommands validation
  -> persisted local expression text
  -> TrackSourceCache dependency graph
  -> SmartListSource parse + compile
  -> SmartListEvaluator over parent membership
  -> ListOrderSource over raw saved rank
  -> ordered derived TrackSource
```

The stored predicate value is expression text, not an AST or execution bytecode.
The same List record independently stores raw rank TrackIds; those IDs do not participate in predicate truth.
Opening or refreshing the List recompiles its expression against the current implementation, then applies rank to the resulting membership.
The text has no separate language identity or version.
Core library storage treats those bytes as opaque and does not make query parsing part of database integrity or `ao::library::kLibraryVersion` admission.
Application authoring may validate a new expression before commit, while source materialization represents stored text that no longer parses or compiles as an expression error with empty membership.
Other retained expression surfaces likewise use the interpretation and compatibility policy of their application owner rather than introducing a per-expression dialect in storage.

### Transient track filter

```text
raw GTK/TUI filter text
  -> UIModel quick-filter resolver
  -> canonical expression text
  -> ViewService.setFilter()
  -> ad-hoc SmartListSource over the base list
  -> replacement TrackListProjection using the existing presentation
```

Changing the filter replaces the membership source while preserving the view's presentation spec.
Changing presentation rebuilds projection shape without changing the filter expression or base membership definition.

### Expression completion

```text
cursor + incomplete expression
  -> core tolerant completion analysis
  -> runtime query completer
       + core variable/operator catalog
       + live tag/custom/value vocabulary
       + optional transient source-text aliases
  -> frontend-neutral replacement range, syntax, typed detail, and rank
  -> UIModel presentation-text resolution
  -> GTK/TUI adapter
```

Metadata-value editors share the runtime vocabulary service but do not become query-language consumers.

### Per-track string evaluation

```text
CLI --format text
  -> shared parser
  -> compileFormat
  -> FormatPlan
  -> FormatBinding + bounded DictionaryReadContext
  -> FormatEvaluator + TrackView
  -> plain output line
```

This path does not create `TrackPresentationSpec`, projection rows, or frontend columns.

## Structural constraints

- Expression text is persistence-facing opaque data; changing whether retained text parses, what it binds to, or which tracks it matches can change application behavior without making its containing database structurally invalid.
- Saved-List predicate interpretation follows the current application query implementation rather than the library database version; library YAML, playback-session, workspace, and CLI surfaces retain their own independent compatibility owners.
- `ExecutionPlan` and `FormatPlan` are runtime-only and are never persisted as compatibility surfaces.
- Plans own every expression symbol needed for later dictionary binding and can outlive the library against which they were first evaluated.
- A dictionary-using plan is bound explicitly for one bounded batch; a later batch creates a new binding and can observe a newer committed dictionary generation.
- Transaction-backed evaluation opens its read snapshot before binding, so batch dictionary resolution is not older than the tracks being evaluated.
- Parser, compiler, completion, variable formatting, serializer, generated CLI help, and diagnostics use the same core variable catalog.
- Application layers generate variable source text through the core query formatter and constants through the core serializer rather than duplicating prefix or quoting rules.
- Smart and transient filters use the same predicate compiler and source evaluation path after authoring policy resolves their input.
- Quick search expansion is UIModel policy, not grammar; direct query entry points receive expression text.
- Quick search emits ordinary `~` expressions and relies on the core language's Unicode-caseless text semantic rather than passing an out-of-band evaluator option.
- Runtime aggregate vocabulary requests carry typed fields selected by UIModel; no Quick-search or frontend role is added to runtime field metadata.
- Completion aliases are transient spelling aids over live vocabulary values; they never enter expression text, query evaluation, library identity, grouping, ordering, LMDB, or YAML.
- Interactive alias matching may suggest an admitted source value, but submitting the unselected romanized input retains the ordinary query and Quick-filter predicate semantics.
- The runtime field catalog may advertise a query-variable bridge only when that typed field resolves in the core query descriptor catalog.
- Query-to-runtime reverse lookup, value-completion eligibility, quick-search construction, and presentation recommendation consume that typed identity instead of maintaining raw variable-name mappings.
- A frontend must not scan `MusicLibrary` to implement ordinary expression evaluation or completion when a runtime service exists.
- The GTK saved-List preview's direct construction of `SmartListEvaluator` against `MusicLibrary` is a contained migration seam, not the normal frontend boundary.

## Failure, cancellation, and lifetime boundaries

Parsing and compilation return typed `Result` failures at their public boundaries.
Saved-List mutation rejects an invalid nonempty expression before commit, while an already materialized source stages an expression error without invalidating sibling sources.
An empty expression compiles as constant true and does not select a different persisted kind.
View filtering publishes the accepted expression, replacement projection, revision, and optional expression error through runtime state.

Completion is synchronous and tolerant of incomplete text.
`CompletionService` caches are owner-thread confined; committed track insertion, mutation, deletion, or library reset invalidates the shared snapshot before its next lazy one-pass rebuild.
Its optional completion-alias policy is borrowed from the interactive composition root, and snapshot rebuild retires every borrowed alias range before replacing the snapshot-owned alias records.
Failure to derive an alias from already-admitted text is an invariant failure rather than a per-entry fallback to mixed matching rules.

Execution and format plans own no dictionary state.
`PlanBinding` and `FormatBinding` borrow their plan and a synchronous `DictionaryReadContext`; those bindings and the context cannot outlive the backing `MusicLibrary`.
The optional dictionary read cache and predicate caseless-key caches are batch-local acceleration over stable borrowed values, not durable plan state or a transaction snapshot.
Callers choose load mode from the compiled access profile, provide every required track tier, and keep the borrowed dictionary context valid for synchronous evaluation.
The evaluator enforces the tier requirement as a caller precondition before predicate shortcuts or format-output mutation; ordinary missing fields within a present tier retain their false or empty-value semantics.
Source leases and projections retain their ordinary lifetime rules from the [library architecture](library.md).

## Implementation map

- [`lib/query/CMakeLists.txt`](../../lib/query/CMakeLists.txt) defines the core expression module and its dependency on `ao_library`.
- [`Parser.h`](../../include/ao/query/Parser.h), [`QueryCompilation.h`](../../include/ao/query/QueryCompilation.h), and [`FormatExpression.h`](../../include/ao/query/FormatExpression.h) define the public parse and compile paths.
- [`PlanEvaluator`](../../include/ao/query/PlanEvaluator.h) and `PlanBinding` evaluate predicates against explicit batch dictionary state.
- [`FormatExpression.h`](../../include/ao/query/FormatExpression.h) defines pure format compilation, `FormatBinding`, and scalar evaluation.
- [`Completion.h`](../../include/ao/query/Completion.h) defines tolerant core completion analysis.
- [`FieldCatalog.h`](../../include/ao/query/FieldCatalog.h) defines typed variable descriptors and lookup.
- [`TrackField`](../../app/include/ao/rt/TrackField.h) defines the application capability catalog and typed query bridge.
- [`LibraryCommands.cpp`](../../app/runtime/library/LibraryCommands.cpp) validates persisted List definitions.
- Source-private [`SmartListSource`](../../app/runtime/source/SmartListSource.h) and [`SmartListEvaluator`](../../app/runtime/source/SmartListEvaluator.h), behind [`TrackSourceCache`](../../app/include/ao/rt/source/TrackSourceCache.h), materialize predicate membership.
- Source-private [`ListOrderSource`](../../app/runtime/source/ListOrderSource.h) applies independent saved rank after predicate evaluation.
- [`ViewService`](../../app/include/ao/rt/ViewService.h) combines base list, transient filter, presentation, and projection state.
- [`CompletionService`](../../app/include/ao/rt/completion/CompletionService.h) and [`QueryExpressionCompleter`](../../app/include/ao/rt/completion/QueryExpressionCompleter.h) compose live runtime completion.
- [`CompletionAliasPolicy`](../../app/include/ao/rt/completion/CompletionAliasPolicy.h) defines the ICU-free alias seam; [`IcuCompletionAliases`](../../app/include/ao/i18n/IcuCompletionAliases.h) supplies the interactive implementation.
- [`resolveTrackFilter`](../../app/include/ao/uimodel/library/track/TrackFilter.h) and [`TrackFilterCompleter`](../../app/include/ao/uimodel/library/track/TrackFilter.h) own shared quick-filter authoring and completion policy.
- [`completionDetail`](../../app/include/ao/uimodel/library/presentation/TrackPresentationText.h) resolves completion roles and counts without changing query syntax.
- [`TrackCommand.cpp`](../../app/cli/TrackCommand.cpp) is the current format-expression consumer.

## Test map

- Query tests under [`test/unit/query/`](../../test/unit/query/) protect parsing, compilation, evaluation, serialization, completion, access profiles, and formatting.
- Smart source tests under [`test/unit/runtime/source/`](../../test/unit/runtime/source/) protect persisted and transient membership materialization.
- Completion tests under [`test/unit/runtime/completion/`](../../test/unit/runtime/completion/) protect live vocabulary composition.
- [`TrackFilterResolutionTest.cpp`](../../test/unit/uimodel/library/track/TrackFilterResolutionTest.cpp) and [`TrackFilterCompleterTest.cpp`](../../test/unit/uimodel/library/track/TrackFilterCompleterTest.cpp) protect quick-filter resolution and completion.
- Presentation recommendation tests under [`test/unit/uimodel/library/presentation/`](../../test/unit/uimodel/library/presentation/) protect the read-only expression-to-preset seam.
- [`CliSmokeTest.cpp`](../../test/unit/cli/CliSmokeTest.cpp) protects CLI filter and format workflows.

## Related documents

- [Library architecture](library.md)
- [Presentation architecture](presentation.md)
- [Track sources](../spec/library/source/track-source.md)
- [Track-list projection](../spec/library/projection/track-list.md)
- [Track-list presentation](../spec/presentation/track-presentation.md)
- [Predicate evaluation](../spec/query/predicate-evaluation.md)
- [Predicate language](../reference/query/predicate-language.md)
- [Format evaluation](../spec/query/format-evaluation.md)
- [Format language](../reference/query/format-language.md)
