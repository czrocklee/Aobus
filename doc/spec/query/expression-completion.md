---
id: query.expression-completion
type: spec
status: current
domain: query
summary: Defines tolerant query-expression completion contexts, ranking, replacement, and live vocabulary composition.
---
# Query expression completion

## Scope

This specification defines completion while editing an incomplete predicate expression.
It owns cursor-context analysis, variable/operator/value/logical suggestions, ranking, insertion, and composition with live library vocabularies.

The complete language surface belongs to the [predicate language reference](../../reference/query/predicate-language.md).
Metadata-editor completion behavior belongs to [track-field value completion](../presentation/field-completion.md), even though both consumers share the runtime vocabulary service.
Classification and completion of a mixed Quick-filter/expression input surface belongs to [track filtering](../presentation/track-filter.md).

## Code boundary

This contract spans the **core libraries**, **application runtime**, and frontend adapter layers from the [system architecture](../../architecture/system-overview.md), as refined by the [track expression](../../architecture/track-expression.md) and [presentation](../../architecture/presentation.md) architectures.
Core analysis is public under `include/ao/query/`; runtime composition is public under `app/include/ao/rt/completion/`; GTK and TUI adapt `CompletionResult` without owning grammar or vocabulary.

## Terminology

- **Completion context** is one of variable token, field operator, value, or logical operator.
- **Partial tail** is the incomplete final token at the cursor that a complete parser cannot accept yet.
- **Replacement range** is the half-open source range replaced when one item is applied.
- **Live vocabulary** is frequency-ranked text derived from the active library.

## Invariants

- Completion accepts incomplete text and does not require a complete AST.
- Core completion analysis does not read the music library or name a frontend control.
- Parser and completion tokenization share lexical token rules for complete tokens.
- A small tolerant scanner handles only partial-tail forms that cannot be lexed as complete parser tokens.
- Completion is blocked inside quoted string content and where a trigger is glued to a preceding identifier.
- Applying a completion replaces exactly the reported range.
- A completion item always inserts the canonical `$` or `@` variable even when an alias triggered it.
- Runtime live values are suggestions only; their absence does not change what the language accepts.

## Completion contexts

### Variables

`$` and `@` suggestions come from the core `QueryVariableDescriptor` catalog.
Canonical names match by ASCII-insensitive prefix.
Aliases match only when the typed alias is complete, and exact-alias matches rank before canonical-prefix matches.
Duplicate canonical results are suppressed.
Display and insertion text is produced by the core variable formatter from the matched canonical descriptor rather than assembled by the runtime.

`#` and `%` suggestions come from the active library's tag and custom-key vocabularies.
Insertion uses the query serializer so names that require quoting produce valid variable text.

### Field operators

After a complete left-hand variable, completion offers the valid operator family for that field:

| Field family | Suggestions |
|---|---|
| String, dictionary, custom | `=`, `!=`, `~`, `in`, `?` |
| Numeric and duration | `=`, `!=`, `<`, `<=`, `>`, `>=`, `in`, `?` |
| Tag | `?` |
| Other scalar fallback | `=`, `!=`, `in`, `?` |

An empty operator prefix is valid after whitespace.
Binary operators replace the typed prefix with a space-padded token; `?` appends as a postfix token without spaces.

### Logical operators

After a complete predicate, completion offers `and`, `or`, `&&`, and `||` by ASCII-insensitive prefix.
An empty prefix after the complete expression is valid.
Prefix negation is not offered at this connection point.

### Values

After a complete binary operator, and inside subsequent elements of an `in [...]` list, completion retains the left-hand query field as context.
Runtime suggestions are currently provided only for query fields whose typed reverse bridge resolves to a value-completable runtime track field.

Insertion serializes the suggested value as a string constant, so a value such as `Massive Attack` becomes `"Massive Attack"`.
Custom values, numeric templates, unit templates, and codec templates are not currently populated.

## State model

`analyzeQueryCompletion(text, cursor)` returns no context when the cursor is out of range, the position is lexically blocked, or no supported connection point exists.
Otherwise it returns one typed context carrying its replacement range and typed prefix.

`QueryExpressionCompleter` visits that context, applies the requested result limit, and returns no `CompletionResult` when no items remain.
Results are ordered by their owning catalog or live-vocabulary rank and receive monotonic runtime rank values in that order.

## Failure and cancellation

Incomplete or invalid expression text is an expected completion state and does not surface a query error.
A failed complete parse can therefore coexist with a useful tolerant completion context.

Completion is synchronous and has no cancellation point.
Live vocabulary access is confined to the `CompletionService` owner thread as defined by [track-field value completion](../presentation/field-completion.md).

## Frontend observations

Runtime returns frontend-neutral display syntax, insertion syntax, a typed detail role or argument, rank, and one replacement range.
Field, alias, operator, logical-operator, and frequency details resolve through the UIModel presentation catalog.
GTK's entry adapter owns popover, keyboard, pointer, and borrowed-entry lifetime behavior.
UIModel's `TrackFilterCompleter` delegates text with an explicit leading query variable to `QueryExpressionCompleter` and otherwise applies Quick-filter completion policy.
GTK's Quick-filter entry and TUI command completion both consume that composition; TUI offsets its nested replacement range into the command line before applying it.

Frontends cannot invent additional query fields or operators that the core completion catalog does not expose.

## Implementation map

- [`Completion.h`](../../../include/ao/query/Completion.h) defines core contexts and completion queries.
- [`FieldCatalog.h`](../../../include/ao/query/FieldCatalog.h) defines the canonical typed variable descriptors and lookup surface.
- [`Completion.cpp`](../../../lib/query/Completion.cpp) owns tolerant analysis and core suggestions.
- [`CompletionTokenizer.cpp`](../../../lib/query/detail/CompletionTokenizer.cpp) adapts shared lexical rules.
- [`QueryExpressionCompleter.cpp`](../../../app/runtime/completion/QueryExpressionCompleter.cpp) composes core analysis with live vocabularies.
- [`PresentationTextCatalog`](../../../app/include/ao/uimodel/presentation/PresentationTextCatalog.h) resolves semantic completion details into shared interactive copy.
- [`TrackFilterCompleter`](../../../app/include/ao/uimodel/library/track/TrackFilterCompleter.h) composes expression completion into the interactive filter surface.
- [`EntryCompletionController`](../../../app/linux-gtk/completion/EntryCompletionController.h) is the GTK adapter.
- [`CommandCompletionProvider`](../../../app/tui/CommandCompletionProvider.h) is the TUI command adapter.

## Test map

- Completion tests under [`test/unit/query/`](../../../test/unit/query/) prove cursor contexts, lexical agreement, operator families, logical boundaries, aliases, values, and latency baseline.
- Runtime completion tests under [`test/unit/runtime/completion/`](../../../test/unit/runtime/completion/) prove live composition and replacement output.
- [`EntryCompletionControllerTest.cpp`](../../../test/unit/linux-gtk/completion/EntryCompletionControllerTest.cpp) and TUI command-completion tests protect adapters.
- [`TrackFilterCompleterTest.cpp`](../../../test/unit/uimodel/library/track/TrackFilterCompleterTest.cpp) protects the mode boundary around this expression completer.

## Related documents

- [Track expression architecture](../../architecture/track-expression.md)
- [Predicate language](../../reference/query/predicate-language.md)
- [Predicate evaluation](predicate-evaluation.md)
- [Track-field value completion](../presentation/field-completion.md)
- [Track filtering](../presentation/track-filter.md)
- [Track field catalog](../../reference/library/model/track-field.md)
