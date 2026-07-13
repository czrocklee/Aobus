---
id: query.predicate-evaluation
type: spec
status: current
domain: query
summary: Defines predicate compilation, track-field truth semantics, access requirements, failures, and consumer behavior.
---
# Predicate evaluation

## Scope

This specification defines how a parsed track expression becomes a boolean `ExecutionPlan` and how that plan evaluates one `library::TrackView`.
The exact grammar, variable names, aliases, operators, and units belong to the [predicate language reference](../../reference/query/predicate-language.md).

Smart-list source ordering, incremental membership, and source errors belong to [track sources](../library/source/track-source.md).
Quick-search input expansion belongs to [track filtering](../presentation/track-filter.md).

## Code boundary

This contract belongs to the **core libraries** layer in the [system architecture](../../architecture/system-overview.md) and is refined by the [track expression architecture](../../architecture/track-expression.md).
Its public compiler and evaluator interfaces live under `include/ao/query/` and its implementation lives under `lib/query/`; runtime consumers supply core library tracks and dictionaries without moving source or presentation ownership into `ao_query`.

## Terminology

- **Predicate** is an expression whose result is boolean.
- **Execution plan** is the runtime-only compiled instruction and constant set.
- **Access profile** is the minimum `TrackView` storage tier required by a plan.
- **Dictionary field** stores an interned id whose text is resolved through `DictionaryStore` where an operation requires text.
- **Existence** is the field-specific presence rule below, not merely successful field lookup.

## Invariants

- Public parse and compile boundaries return `Result` and do not throw for malformed user input.
- A successfully compiled plan has boolean result semantics.
- A bare non-tag variable cannot compile as a predicate.
- A bare tag variable and its explicit postfix-existence form both test tag membership.
- Equality, inequality, ordering, substring, list membership, and range membership are case-sensitive for string values.
- `in [a, b]` is equivalent to equality against one listed constant.
- `in lower..upper` is an inclusive closed range.
- Predicate evaluation never changes the track, dictionary, source, or presentation state.
- A plan is runtime-only; persisted Smart Lists retain their expression text and recompile it.

## Field semantics

### Existence

| Field kind | Exists when |
|---|---|
| Title and URI text | The string is non-empty. |
| Dictionary metadata | The dictionary id is not invalid. |
| Year, track/disc/movement numbers and totals | The stored value is greater than zero. |
| Duration, bitrate, sample rate, channels, and bit depth | The stored value is greater than zero. |
| Codec | The codec is not `UNKNOWN`. |
| Cover art | Primary cover selection yields a valid resource id. |
| Tag | The track contains the named tag id. |
| Custom metadata | The named key is present, even if its value is empty. |

Dictionary metadata includes artist, album, album artist, genre, composer, conductor, ensemble, work, movement, and soloist.
The canonical missing-value form is a negated existence predicate such as `!$year?`.

### Comparison

Plain text and custom metadata comparisons use the stored string.
Equality and `in` lists over dictionary metadata resolve the expression constant to its dictionary id and compare ids.
Substring and ordered comparisons over dictionary metadata resolve the track id back to text.

`~` performs case-sensitive substring containment.
Ordered text comparison is case-sensitive lexicographic order.
Numeric fields compare their scaled integer values.
Codec constants compile to their stored codec values.

### Lists and ranges

List elements may mix literal kinds; every element is compiled using the ordinary equality conversion for the left field.
The compiler may use a set representation without changing equality semantics.

Ranges are inclusive.
Dictionary-backed ranges require string bounds and compare resolved text rather than dictionary insertion order.

### Units

Unit literals are scaled in the context of the left field before evaluation.
Duration uses milliseconds as its plan value, bitrate uses bits per second, and sample rate uses hertz.
A scaled result that is not an integer is rejected during compilation.

## Access profile

Compilation classifies a plan as `NoTrackData`, `HotOnly`, `ColdOnly`, or `HotAndCold`.
The profile is the union of the storage tiers required by all field loads in the predicate.

`PlanEvaluator::matches()` returns false when the supplied `TrackView` lacks a required tier.
Smart-list evaluation uses the plan profile to choose the minimum track-store load mode across the plans evaluated in one batch.
Constant true and false predicates require no track data.

## Commands and transitions

The public stages are:

1. `parse(text)` produces an AST or `FormatRejected`.
2. `compileQuery(ast, dictionary)` produces an `ExecutionPlan` or `FormatRejected`.
3. `PlanEvaluator::matches(plan, track)` returns one membership decision.

`QueryCompiler::compile()` has the same non-throwing result contract as `compileQuery()`.
Unknown `$` or `@` fields are rejected with a diagnostic generated from the shared field catalog and may include a close-name suggestion.

`SmartListSource` parses an empty expression as the constant `true` for source evaluation.
`LibraryWriter` treats an empty Smart List expression as no smart expression at the list-definition boundary, so list kind semantics remain owned by the library contracts.

## Failure and cancellation

Syntax and semantic failures use `Error::Code::FormatRejected` with a human-readable message.
The parser reports the expression as a whole rather than a stable source offset.
Semantic compilation provides narrower diagnostics for unknown fields, invalid units, unsupported operand types, bare fields, and non-predicate AST shapes.

The recursive compiler may use private exceptions internally, but the public compile boundary translates them to `Result`.
Presence-only field probes used by completion return absence rather than a user-facing error.

Predicate compilation and evaluation are synchronous and have no cancellation point.
Longer-running source rebuild cancellation and atomic publication belong to the consuming runtime source contract.

## Persistence and versioning

Execution plans and internal field opcodes are not persisted.
Smart List records store local expression text, while runtime view filters retain expression text in view/session state according to their owning contracts.

A language or semantic change must consider existing stored expressions even when no storage-version change is required.
Removing accepted text or changing its truth result requires coordinated reference, source, persistence, and behavior tests.
[RFC 0024](../../rfc/0024-versioned-predicate-dialect.md) proposes an explicit dialect dispatch and migration boundary; current expressions remain unversioned.

## Frontend observations

The core evaluator exposes no frontend state.
Runtime consumers translate failures and membership into their own typed observations:

- `SmartListSource` stages an expression error and exposes empty membership for that invalid state.
- `ViewService` publishes filter status and a replacement projection.
- CLI filter commands prefix and print the returned diagnostic.

Presentation consumes resulting membership but cannot reinterpret predicate truth.

## Implementation map

- [`QueryCompiler.h`](../../../include/ao/query/QueryCompiler.h) and [`ExecutionPlan.h`](../../../include/ao/query/ExecutionPlan.h) define compilation and the opaque plan boundary.
- [`PlanEvaluator.h`](../../../include/ao/query/PlanEvaluator.h) defines evaluation.
- [`ExecutionPlan.cpp`](../../../lib/query/ExecutionPlan.cpp) owns semantic compilation and access profiles.
- [`PlanEvaluator.cpp`](../../../lib/query/PlanEvaluator.cpp) owns track evaluation.
- [`SmartListSource.cpp`](../../../app/runtime/source/SmartListSource.cpp) adapts expressions into runtime source state.
- [`SmartListEvaluator.cpp`](../../../app/runtime/source/SmartListEvaluator.cpp) batches plan evaluation over source changes.

## Test map

- Execution-plan tests under [`test/unit/query/`](../../../test/unit/query/) prove semantic compilation, field resolution, lists, ranges, units, access profiles, and failures.
- Plan-evaluator tests under the same directory prove field truth, existence, comparison, tag, custom metadata, and dictionary behavior.
- Smart-list evaluator tests under [`test/unit/runtime/source/`](../../../test/unit/runtime/source/) prove profile-aware batch consumption and membership publication.

## Related documents

- [Track expression architecture](../../architecture/track-expression.md)
- [Predicate language](../../reference/query/predicate-language.md)
- [Track sources](../library/source/track-source.md)
- [Track filtering](../presentation/track-filter.md)
- [Track model](../../reference/library/model/track.md)
- [RFC 0024: versioned predicate dialect](../../rfc/0024-versioned-predicate-dialect.md)
