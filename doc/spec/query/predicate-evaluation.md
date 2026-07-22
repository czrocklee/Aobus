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
- **Plan binding** resolves one plan's owned dictionary symbols for one bounded evaluation batch.
- **Dictionary read context** is the synchronous committed lookup/text port borrowed by a binding and evaluator.
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
- Parsing and compilation do not read or mutate a library dictionary; a plan owns all symbol text required for later binding.
- An execution plan contains no library or dictionary pointer.
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
Only a direct string-constant operand contributes a dictionary symbol to its comparison; a nested comparison contributes its scalar boolean result and cannot leak an inner literal into the parent instruction.

An unresolved tag never matches membership or existence.
When a current binding cannot resolve a custom key or dictionary equality constant, equality and membership do not match while inequality matches.
An unresolved custom key also makes existence, ordered, and substring comparisons non-matching rather than treating the missing key as an empty stored value.
A later binding may resolve the same plan-owned symbol after a committed dictionary generation advance.

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

Supplying every tier required by the plan is a caller precondition of `PlanEvaluator::matches()` and `evaluateFull()`.
The evaluator enforces that contract before any constant-plan shortcut; absence of an individual field within a supplied tier remains ordinary predicate data and follows the field semantics above.
Smart-list evaluation uses the plan profile to choose the minimum track-store load mode across the plans evaluated in one batch.
Constant true and false predicates require no track data.

## Commands and transitions

The public stages are:

1. `parse(text)` produces an AST or `FormatRejected`.
2. `compileQuery(ast)` produces a dictionary-independent `ExecutionPlan` or `FormatRejected`.
3. For a dictionary-using plan, `PlanBinding(plan, context)` resolves its symbols under one committed dictionary lock and records that generation.
4. `PlanEvaluator::matches(binding, track)` returns one membership decision.

`QueryCompiler::compile()` has the same non-throwing result contract as `compileQuery()`.
Unknown `$` or `@` fields are rejected with a diagnostic generated from the shared field catalog and may include a close-name suggestion.
Plans with no dictionary access may use the context-free convenience overloads.
Supplying an explicit `DictionaryReadContext` or bound plan is a precondition for plans whose `requiresDictionary` flag is true.

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

The boolean evaluation API has no insufficient-data result.
Callers that choose load mode must use the plan access profile and provide the required hot/cold tiers; violating that precondition is a caller contract failure, not a non-match.
Ordinary missing values inside a present tier remain non-matching according to the field rules.

## Persistence and versioning

Execution plans and internal field opcodes are not persisted.
Smart List records store local expression text, while runtime view filters retain expression text in view/session state according to their owning contracts.

A predicate change must consider retained expressions even when no record byte changes.
For Smart Lists, accepted grammar, field binding, and the truth behavior in this specification are part of the library database contract gated by `ao::library::kLibraryVersion`.
A change that expands the storable predicate surface beyond what an existing same-version reader accepts, or that can alter whether stored text parses or compiles, what it binds to, or which tracks it matches, must increment that version.
The old version is then rejected or explicitly migrated; the current database accepts only an exact version match and provides no in-place migration.

Runtime view filters use the compatibility policy of the workspace, playback-session, or CLI surface that retains or accepts them.
Expressions carry no independent dialect id or version; the containing surface owns compatibility.

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
- [`DictionaryStore.h`](../../../include/ao/library/DictionaryStore.h) defines bounded dictionary contexts, caches, and committed generation.
- [`ExecutionPlan.cpp`](../../../lib/query/ExecutionPlan.cpp) owns semantic compilation and access profiles.
- [`PlanEvaluator.cpp`](../../../lib/query/PlanEvaluator.cpp) owns track evaluation.
- [`SmartListSource.cpp`](../../../app/runtime/source/SmartListSource.cpp) adapts expressions into runtime source state.
- [`SmartListEvaluator.cpp`](../../../app/runtime/source/SmartListEvaluator.cpp) batches plan evaluation over source changes.

## Test map

- Execution-plan tests under [`test/unit/query/`](../../../test/unit/query/) prove semantic compilation, field resolution, lists, ranges, units, access profiles, and failures.
- Plan-evaluator tests under the same directory prove field truth, existence, comparison, nested-comparison symbol isolation, tag, custom metadata, and dictionary behavior.
- Smart-list evaluator tests under [`test/unit/runtime/source/`](../../../test/unit/runtime/source/) prove profile-aware batch consumption, membership publication, and rebinding of plan symbols introduced by a later dictionary commit.

## Related documents

- [Track expression architecture](../../architecture/track-expression.md)
- [Predicate language](../../reference/query/predicate-language.md)
- [Track sources](../library/source/track-source.md)
- [Track filtering](../presentation/track-filter.md)
- [Track model](../../reference/library/model/track.md)
- [Library database](../../reference/library/storage/database.md)
