---
id: rfc.0009.pure-expression-binding
type: rfc
status: draft
domain: query
summary: Proposes non-mutating expression compilation, owned symbols, explicit evaluation context, and safe dictionary lifetime.
depends-on: none
---
# RFC 0009: Pure expression binding and evaluation context

## Implementation status

The non-mutating binding foundation was implemented on 2026-07-15 as part of [RFC 0022](0022-transaction-coherent-library-dictionary.md):

- query and format compilation takes no dictionary and owns all symbol text in the resulting plan;
- plans retain no `DictionaryStore` pointer;
- `DictionaryReadContext` provides bounded synchronous id/text access and one-lock symbol binding with a process-local committed generation;
- `PlanBinding` and `FormatBinding` resolve one plan once per evaluation batch;
- a later committed dictionary delta advances the generation, and a newly created binding can resolve symbols that were unknown when the plan was compiled; and
- the global mutating `getOrIntern()` expression path was removed.

This RFC remains `draft` because typed insufficient-data/context outcomes, explicit expression resource limits, and immutable worker-valid dictionary snapshots are not implemented.
Current boolean and string convenience APIs still return `false` or empty output when required track tiers are absent, and the borrowed synchronous context must not outlive its dictionary.
The [track expression architecture](../architecture/track-expression.md), [predicate evaluation specification](../spec/query/predicate-evaluation.md), and [format evaluation specification](../spec/query/format-evaluation.md) own the implemented subset.

## Problem

Before the implemented binding foundation, query and format compilation resolved tag names, custom keys, and dictionary-backed constants through `DictionaryStore::getOrIntern()`.
Compilation that appears read-only therefore allocates dictionary ids, grows in-memory string storage, and retains `_reservedStrings` that may never be persisted or used by track data.
Repeated transient expressions can reserve unbounded dictionary entries.

The behavior has one useful property: if a previously unknown value is later written, `DictionaryStore::put()` reuses the reserved id, allowing an existing numeric plan to match it.
That optimization couples expression lifetime to mutable dictionary allocation and makes a parsing/compilation path an implicit state mutation.

Those `ExecutionPlan` and `FormatPlan` values also retained a raw `DictionaryStore` pointer.
The type system does not prove that the dictionary outlives the plan or that an evaluation occurring on another executor has a valid library context.
This blocks a clean worker-safe implementation for [RFC 0006](0006-coherent-derived-track-views.md).

Finally, evaluation reports missing required `TrackView` tiers as ordinary `false` for predicates or an empty string for formats.
Those results are indistinguishable from a legitimate non-match or empty formatted value when the API is used outside its current well-formed runtime paths.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Make parsing and compilation observationally read-only with respect to library storage and in-memory dictionary allocation.
- Let plans own every expression symbol needed for later binding.
- Remove raw dictionary pointers from plan values.
- Preserve the ability of a long-lived Smart List to match dictionary values introduced after compilation.
- Support worker-safe plan transfer and evaluation with explicit lifetime and generation evidence.
- Keep known-id equality, tag bloom, and set optimizations where evidence proves they are valid.
- Distinguish insufficient track data from a valid predicate or format result.
- Bound symbol and cache memory for adversarial transient expressions.

## Non-goals

- Changing predicate or format language syntax.
- Persisting execution or format plans.
- Moving source membership or presentation into the core query library.
- Replacing the library dictionary storage format.
- Defining asynchronous derived-view scheduling; [RFC 0006](0006-coherent-derived-track-views.md) owns that runtime transaction.
- Unifying query and runtime track fields; [RFC 0008](0008-declarative-track-capability-bridge.md) owns that bridge.

## Proposed design

### Symbol-owning plans

Compiled plans retain canonical symbol text independently of a dictionary:

```cpp
using ExpressionSymbolIndex = std::uint32_t;

struct ExpressionSymbol final
{
  QuerySymbolKind kind;
  std::string text;
};

struct ExecutionPlan final
{
  // bytecode and constants
  std::vector<ExpressionSymbol> symbols;
  AccessProfile accessProfile;
};
```

Tag names, custom keys, and dictionary-backed string constants compile to symbol indexes rather than forcing an id allocation.
Repeated equal symbols deduplicate within the plan.

`FormatPlan` uses the same symbol representation for dictionary fields and custom keys.
Neither plan contains `DictionaryStore*`, references into parser input, or views into runtime-owned strings.

### Read-only dictionary context

Core library exposes a read-only context for one evaluation period:

```cpp
struct DictionaryReadContext final
{
  DictionaryGeneration generation;
  std::optional<DictionaryId> lookupId(std::string_view text) const;
  std::string_view lookupText(DictionaryId id) const;
};
```

The concrete context may borrow a guarded dictionary only for a synchronous call or own an immutable snapshot for worker execution.
Its lifetime is explicit at the evaluator boundary and never hidden inside a plan.

`DictionaryStore` advances `DictionaryGeneration` when its visible id/text mapping changes.
Loading a library establishes an initial generation.
Expression compilation uses no dictionary mutation and generally needs no dictionary argument.

### Generation-aware binding cache

The implemented `PlanBinding` and `FormatBinding` resolve `symbol index -> optional DictionaryId` once for one bounded evaluation batch and record the committed generation used for that resolution.
Consumers create a new binding for a later batch, so a generation advance is observed without recompiling the immutable plan.

This preserves future matching:

1. Compile `#future` while no dictionary entry exists; the plan retains `"future"`.
2. Evaluation at generation N resolves no id and returns no membership.
3. A later library mutation persists `future` and advances the dictionary generation.
4. Evaluation at generation N+1 resolves the new id and can match without recompiling the expression.

Unknown custom keys and dictionary constants follow the same rule.

The plan remains immutable and shareable.
Bindings and mutable evaluator registers are not shared implicitly; each concurrent worker needs its own evaluator and worker-valid context/binding.

### Comparison and tag behavior

Known-id equality and `in` sets retain numeric fast paths after binding.
Unknown symbols remain non-matching until a context resolves them.

Substring and ordered dictionary comparisons already require id-to-text lookup and continue through the explicit context.
Tag bloom rejection is used only after every required tag symbol has a bound id for the current generation; otherwise evaluation skips the bloom shortcut and performs the correct full check.

Custom metadata instructions bind their key id through the current context.
Format evaluation appends empty text when the key remains unknown or absent, matching current value semantics without reserving an id.

### Explicit evaluation outcome

Introduce a typed result that separates valid evaluation from missing input:

```cpp
enum class EvaluationStatus
{
  Value,
  InsufficientTrackData,
  InvalidContext,
};

template<typename T>
struct EvaluationOutcome final
{
  EvaluationStatus status;
  T value;
};
```

The exact API may use `Result`, but callers must be able to distinguish insufficient hot/cold tiers and invalid dictionary context from `false` or an empty string.

Runtime smart-source code treats insufficient data as an internal load-contract failure because it chooses storage mode from the plan access profile.
CLI reports a typed failure rather than silently printing an empty line when its read mode is wrong.

Convenience boolean/string wrappers may remain only where their preconditions are enforced and named explicitly.

### Resource limits

Parser and compiler apply existing expression-size/instruction limits or introduce explicit limits for symbol count and total symbol bytes.
Evaluator binding caches are proportional to the plan's bounded symbol table and one dictionary generation.

No global dictionary allocation occurs from rejected, previewed, or transient expressions.

### Worker integration

[RFC 0006](0006-coherent-derived-track-views.md) workers receive an immutable plan plus a worker-valid `DictionaryReadContext` or snapshot tied to the captured library revision.
The callback executor revalidates library/dictionary generation before installing results.

The worker cannot retain a raw pointer to `MusicLibrary` or `DictionaryStore` after its task-lifetime token expires.
Shutdown waits for contexts/snapshots before destroying their backing library state.

## Alternatives

### Keep `getOrIntern()` and cap reserved strings

A cap bounds abuse but compilation remains mutating and plan lifetime remains coupled to a raw dictionary pointer.
It can be an emergency mitigation, not the selected architecture.

### Persist every interned expression symbol immediately

This turns arbitrary searches into durable library writes, revision noise, and database growth.
It is worse than the current in-memory reservation.

### Recompile every Smart List after dictionary mutation

Recompilation can restore future-value matching but multiplies work across all active lists and still needs safe dictionary lifetime.
Generation-aware evaluation resolves only symbols used by the evaluated plan.

### Copy the entire dictionary into every plan

This makes plans self-contained but has unacceptable memory cost and remains stale when new values appear.

### Store shared ownership of `DictionaryStore` in plans

Shared ownership prevents dangling pointers but retains mutable interning and makes storage lifetime follow arbitrary plan copies.
Explicit evaluation context keeps ownership at the runtime composition boundary.

### Keep false/empty on insufficient data

Current runtime callers normally load the correct tiers, but silent fallback makes future consumers and worker migrations fail closed as valid content rather than expose a contract violation.
Typed outcomes are safer.

## Compatibility and migration

No persisted expression or library schema change is required.
Plans are runtime-only and may change representation freely.

Implementation phases are:

1. **Complete:** add non-mutating optional dictionary lookup and dictionary generation APIs.
2. **Complete:** add plan-owned symbols and differential behavior tests.
3. **Complete:** move tag, custom-key, dictionary constant, and format binding to explicit batch bindings backed by read contexts.
4. **Complete:** remove `DictionaryStore*` from plan types and remove compiler calls to `getOrIntern()`.
5. Introduce typed insufficient-data/context outcomes and migrate runtime/CLI consumers.
6. Enable RFC 0006 worker evaluation using immutable or task-scoped dictionary contexts.
7. **Complete:** remove `getOrIntern()` because no durable or expression owner remains.

The completed binding phases use differential field/operator coverage and old/new-generation binding tests to protect semantic equivalence.

## Validation

- Parsing and compiling arbitrary query/format text leaves dictionary size, free ids, reserved strings, LMDB rows, and library revision unchanged.
- Repeated unique transient expressions have memory bounded by their plan lifetimes and configured expression limits.
- A plan compiled before a tag, custom key, or dictionary value exists begins matching after that value is persistently introduced and the evaluation context generation advances.
- Known-id equality, set membership, tag bloom, substring, and ordered comparisons remain behavior-equivalent to current tests.
- Format output remains identical for known, absent, zero, and `UNKNOWN` values.
- Destroying a library before a retained plan is safe because the plan owns no raw library pointer; evaluation without a valid context returns a typed outcome.
- Hot-only, cold-only, and mixed insufficient views are distinguishable from valid false/empty results.
- Concurrent evaluator instances may use the same immutable plan without sharing registers or mutable binding cache.
- Worker evaluation, library switching, and shutdown pass ThreadSanitizer and stale-generation tests.
- Query performance baselines quantify binding-cache overhead and preserve accepted hot-path budgets.
- Completed implementation passes `./ao check`.

## Open questions

- Should typed evaluation outcome replace the current methods directly or be introduced beside preconditioned fast wrappers?
- What expression symbol-count and byte limits are appropriate for Smart Lists and CLI input?
- What immutable dictionary snapshot or task-scoped context should worker execution use?

## Promotion plan

If accepted and implemented, update:

- [Track expression architecture](../architecture/track-expression.md) with symbol ownership, context lifetime, and worker safety;
- [Predicate evaluation](../spec/query/predicate-evaluation.md) with binding generations and typed outcomes;
- [Format evaluation](../spec/query/format-evaluation.md) with context and insufficient-data behavior;
- [Predicate language](../reference/query/predicate-language.md) and [format language](../reference/query/format-language.md) if resource limits become user-visible validation;
- [Library architecture](../architecture/library.md) if dictionary generation/context ownership becomes a library responsibility;
- runtime execution guidance for worker-scoped read contexts.
