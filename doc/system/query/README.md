---
id: architecture.track-expression
---
# Track expression architecture

## Find the contract for your change

- [Predicate compilation and evaluation](predicate-evaluation.md)
- [Scalar format evaluation](format-evaluation.md)
- [Tolerant completion and replacement](expression-completion.md)
- [Predicate language](../../reference/query/predicate-language.md)
- [Format language](../../reference/query/format-language.md)

## Scope

Track expressions are the shared Core mechanism by which text becomes a track predicate, an editing-completion context, or a per-track scalar string.
This page explains that structural boundary and how it composes with library sources, runtime services, presentation, and frontends.
The linked language references own exact syntax and names; the evaluation and completion topics own truth, output, and completion behavior.

## Layer boundary

```text
GTK / TUI input
  -> UIModel authoring policy
  -> runtime source or completion service
  -> ao::query parser/compiler/evaluator
  -> ao::library::TrackView

CLI filter or format input ---------^
TUI terminal-title format ----------^
```

The public `ao_query` Core library lives under `include/ao/query/` and `lib/query/`.
It depends on Core library track and dictionary values, not on runtime, UIModel, or a frontend.
Application composition belongs to runtime, platform-neutral quick-filter and recommendation policy belongs to UIModel, and native controls remain adapters.

## Shared expression core

The parser accepts a common syntactic superset.
The predicate and format compilers then enforce distinct result types:

- `ExecutionPlan` answers whether one track matches.
- `FormatPlan` produces one string from one track.
- tolerant completion analyzes incomplete predicate text without creating a second grammar.

Plans are runtime-only and own the expression symbols required for later binding.
Compilation reads no library dictionary and retains no library pointer.
Evaluation that needs dictionary values receives an explicit bounded read context and a plan-specific binding.
Bindings and Unicode-caseless caches are batch-local; no derived key is persisted.
The parser, compiler, serializer, completion logic, diagnostics, and application bridges share the typed Core field catalog rather than maintaining independent variable-name tables.

## Application composition

### Predicate consumers

Saved Lists persist expression text, not an AST or bytecode.
`LibraryCommands` validates authored List expressions before commit; source machinery recompiles stored text and evaluates membership over an upstream source.
Saved rank is an independent overlay applied after predicate membership.
Transient view filters use the same source evaluation path without creating a persisted List.

Quick Filter is UIModel authoring policy, not a dialect.
It emits ordinary Core expression text and cannot change predicate truth through a hidden evaluator option.
Presentation may inspect typed expression fields to recommend a view shape, but it cannot reinterpret membership.

### Completion consumers

Core provides tolerant cursor analysis and the static field/operator catalog.
Runtime combines that analysis with one source-preserving snapshot of live titles, tags, custom keys, and dictionary-backed values.
Optional romanized aliases are transient matching aids: completion still displays and inserts admitted source text, and aliases never become expression syntax, identity, ordering, or storage.
GTK and TUI adapt the resulting replacement range and item metadata; WinUI currently uses the shared filter resolution policy without exposing completion candidates.

### Format consumers

The CLI `track show --format` command compiles an expression and emits one scalar line for each readable selected track.
The TUI `terminalTitleFormat` preference uses the same parser and `FormatPlan`; an empty preference disables the custom title.
The TUI evaluates the current playing track through its runtime library facade, then owns terminal-title sanitization, animation composition, and terminal escape output.
Neither consumer turns a format expression into columns, grouping, sort order, or a filesystem path.

## Membership and presentation remain separate

```text
list id + predicate text -> TrackSource membership
saved rank ids           -> source order
TrackPresentationSpec    -> sort, grouping, visible fields
source + presentation    -> TrackListProjection
```

Expressions can determine membership or one scalar string.
They never define projection rows, grouping, sorting, visible columns, or frontend layout.
A projection consumes an already-materialized source and a presentation specification; it does not parse expressions.

## Persistence and compatibility

Core library storage treats saved expression bytes as opaque scalar-valid text and does not use the query grammar for database admission.
Opening or refreshing a saved List recompiles the text using the current application implementation.
A grammar, binding, or truth change can therefore produce a source expression error or different membership without making the database structurally corrupt.
Other retained surfaces—workspace/session state, TUI preferences, and automation—own their own compatibility policy.
Expressions carry no independent dialect id.

Plans, bindings, opcodes, and completion caches are not persisted.
A dictionary-using plan binds once for a bounded batch; a later binding may observe a newer committed dictionary generation.
For transaction-backed tracks, callers open the read snapshot before binding so dictionary resolution is not older than the evaluated rows.

## Failure, cancellation, and lifetime

Parsing and compilation return typed failures for malformed user input.
Predicate and format evaluation are synchronous and have no cancellation point; longer source rebuild cancellation belongs to runtime.
Supplying every hot/cold track tier named by a plan's access profile is a caller precondition, while an absent value inside a supplied tier follows ordinary false or empty semantics.

Completion is synchronous and tolerant of incomplete text.
Its runtime vocabulary cache is owner-thread confined and invalidated by committed track changes or library reset.
Bindings borrow their plan and dictionary context and cannot outlive either or the backing library.

## Source boundaries

- Core parse, compile, evaluation, serialization, completion, and field-catalog APIs: [`include/ao/query/`](../../../include/ao/query) and [`lib/query/`](../../../lib/query).
- Runtime source and completion composition: [`app/runtime/source/`](../../../app/runtime/source) and [`app/runtime/completion/`](../../../app/runtime/completion).
- UIModel filter policy: [`TrackFilter.h`](../../../app/include/ao/uimodel/library/track/TrackFilter.h).
- Scalar consumers: [`TrackCommand.cpp`](../../../app/cli/TrackCommand.cpp), [`TerminalTitleFormat.cpp`](../../../app/tui/TerminalTitleFormat.cpp), and [`TerminalTitle.cpp`](../../../app/tui/TerminalTitle.cpp).

Detailed symbol and test inventories remain with the linked language and behavior pages.

## Related documents

- [Library architecture](../library/structure.md)
- [Track sources](../library/track-source.md)
- [Presentation architecture](../presentation/README.md)
- [Track filtering](../presentation/track-filter.md)
- [Track-list presentation](../presentation/track-presentation.md)
- [Predicate evaluation](predicate-evaluation.md)
- [Format evaluation](format-evaluation.md)
- [Expression completion](expression-completion.md)
