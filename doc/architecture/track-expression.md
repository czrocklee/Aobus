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

It does not define the exact expression grammar, predicate truth rules, completion ranking, format output, smart-list membership behavior, or track-list presentation behavior.
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

`ao::query` owns the parser, AST, serializer, variable catalog, field resolution, tolerant completion analysis, predicate compiler and evaluator, and format compiler and evaluator.
The parser accepts the common syntactic superset while the predicate and format compilers enforce distinct result types.

An `ExecutionPlan` answers whether one track matches.
A `FormatPlan` produces one string from one track.
Neither plan owns source membership, presentation shape, or frontend state.

### Library and runtime consumers

`LibraryWriter` validates a smart-list expression before committing its text as part of a list definition.
`TrackSourceCache`, `SmartListSource`, and `SmartListEvaluator` compile that text and maintain the resulting ordered membership over an upstream source.
The same source machinery materializes transient `ViewService` filters without persisting a new list.
During one membership rebuild, `SmartListEvaluator` may share a library-owned dictionary read cache across plan evaluations; cache presence and eviction do not change predicate results.

`CompletionService` owns live library vocabularies and `QueryExpressionCompleter` combines them with the core completion analysis.
The runtime does not redefine expression grammar or field aliases.

### UIModel authoring and recommendation

UIModel owns platform-neutral interpretation of quick-filter input, filter view state, Smart List draft and preview policy, and track-presentation recommendation.
It may parse or serialize a core expression, but it cannot implement a second grammar or evaluator.

Presentation recommendation may inspect which variables occur in a valid Smart List expression.
That inspection selects a view shape only; it never changes membership or expression meaning.

### Frontend adapters

GTK and TUI bind raw input, completion results, filter state, and Smart List editing to their native interaction models.
They send the resolved expression to runtime rather than evaluating tracks themselves.

The CLI uses the core predicate path for `--filter` and the core string path for `track show --format`.
The format path currently terminates at plain CLI output and is not a track-list presentation mechanism.

## Boundaries and dependency direction

The content and shape axes of a runtime track-list view remain independent:

```text
listId + filterExpression
  -> TrackSource membership

TrackPresentationSpec
  -> sort + group + visible fields

TrackSource + TrackPresentationSpec
  -> LiveTrackListProjection
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

### Persisted Smart List

```text
frontend editor or CLI command
  -> UIModel draft when interactive
  -> LibraryWriter validation
  -> persisted local expression text
  -> TrackSourceCache dependency graph
  -> SmartListSource parse + compile
  -> SmartListEvaluator over parent membership
  -> ordered derived TrackSource
```

The stored value is expression text, not an AST or execution bytecode.
Opening or refreshing the Smart List recompiles it against the current implementation.
The text has no separate language identity or version.
For a persisted Smart List, the accepted grammar, field catalog, binding behavior, and evaluation meaning are part of the library database contract gated by `ao::library::kLibraryVersion`, even when the list-record byte layout is unchanged.
Other retained expression surfaces use the compatibility version or policy owned by their containing format rather than introducing a per-expression dialect.

### Transient track filter

```text
raw GTK/TUI filter text
  -> UIModel quick-filter resolver
  -> canonical expression text
  -> ViewService.setFilter()
  -> ad-hoc SmartListSource over the base list
  -> replacement LiveTrackListProjection using the existing presentation
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
  -> frontend-neutral replacement range and items
  -> GTK/TUI adapter
```

Metadata-value editors share the runtime vocabulary service but do not become query-language consumers.

### Per-track string evaluation

```text
CLI --format text
  -> shared parser
  -> FormatCompiler
  -> FormatPlan
  -> FormatEvaluator + TrackView
  -> plain output line
```

This path does not create `TrackPresentationSpec`, projection rows, or frontend columns.

## Structural constraints

- Expression text is persistence-facing; changing the storable predicate surface or altering whether retained text parses, what it binds to, or which tracks it matches is incompatible for every containing persistence or automation contract that retains that text.
- Smart List compatibility is gated by the library database version; the library YAML, playback-session, workspace, and CLI surfaces retain their own independent compatibility owners.
- `ExecutionPlan` and `FormatPlan` are runtime-only and are never persisted as compatibility surfaces.
- Parser, compiler, completion, serializer, generated CLI help, and diagnostics use the same core variable catalog.
- Smart and transient filters use the same predicate compiler and source evaluation path after authoring policy resolves their input.
- Quick search expansion is UIModel policy, not grammar; direct query entry points receive expression text.
- The runtime field catalog may advertise a query-variable bridge only when that variable is accepted by the core query catalog.
- A frontend must not scan `MusicLibrary` to implement ordinary expression evaluation or completion when a runtime service exists.
- The GTK Smart List preview's direct construction of `SmartListEvaluator` against `MusicLibrary` is a contained migration seam, not the normal frontend boundary.

## Failure, cancellation, and lifetime boundaries

Parsing and compilation return typed `Result` failures at their public boundaries.
Smart-list mutation rejects an invalid expression before commit, while an already materialized source stages an expression error without invalidating sibling sources.
View filtering publishes the accepted expression, replacement projection, revision, and optional expression error through runtime state.

Completion is synchronous and tolerant of incomplete text.
`CompletionService` caches are owner-thread confined and are invalidated by library changes before their next lazy rebuild.

Execution and format plans borrow dictionary state when required and cannot outlive the library composition that owns that dictionary.
An evaluator dictionary cache is an optional batch-local acceleration over stable borrowed values, not plan state or a dictionary snapshot.
Source leases and projections retain their ordinary lifetime rules from the [library architecture](library.md).

## Implementation map

- [`lib/query/CMakeLists.txt`](../../lib/query/CMakeLists.txt) defines the core expression module and its dependency on `ao_library`.
- [`Parser.h`](../../include/ao/query/Parser.h), [`QueryCompiler.h`](../../include/ao/query/QueryCompiler.h), and [`FormatExpression.h`](../../include/ao/query/FormatExpression.h) define the public parse and compile paths.
- [`PlanEvaluator`](../../include/ao/query/PlanEvaluator.h) evaluates predicates and optionally consumes a matching batch-local dictionary read cache.
- [`Completion.h`](../../include/ao/query/Completion.h) defines tolerant core completion analysis.
- [`LibraryWriter.cpp`](../../app/runtime/library/LibraryWriter.cpp) validates persisted Smart List definitions.
- [`SmartListSource`](../../app/include/ao/rt/source/SmartListSource.h), [`SmartListEvaluator`](../../app/include/ao/rt/source/SmartListEvaluator.h), and [`TrackSourceCache`](../../app/include/ao/rt/source/TrackSourceCache.h) materialize predicate membership.
- [`ViewService`](../../app/include/ao/rt/ViewService.h) combines base list, transient filter, presentation, and projection state.
- [`CompletionService`](../../app/include/ao/rt/completion/CompletionService.h) and [`QueryExpressionCompleter`](../../app/include/ao/rt/completion/QueryExpressionCompleter.h) compose live completion.
- [`TrackFilterResolver`](../../app/include/ao/uimodel/library/track/TrackFilterResolver.h) owns quick-filter authoring policy.
- [`TrackCommand.cpp`](../../app/cli/TrackCommand.cpp) is the current format-expression consumer.

## Test map

- Query tests under [`test/unit/query/`](../../test/unit/query/) protect parsing, compilation, evaluation, serialization, completion, access profiles, and formatting.
- Smart source tests under [`test/unit/runtime/source/`](../../test/unit/runtime/source/) protect persisted and transient membership materialization.
- Completion tests under [`test/unit/runtime/completion/`](../../test/unit/runtime/completion/) protect live vocabulary composition.
- [`TrackFilterResolverTest.cpp`](../../test/unit/uimodel/library/track/TrackFilterResolverTest.cpp) protects quick-filter resolution.
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
- [RFC 0024: versioned predicate dialect](../rfc/0024-versioned-predicate-dialect.md), rejected in favor of containing-surface version ownership
