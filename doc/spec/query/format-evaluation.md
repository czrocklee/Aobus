---
id: query.format-evaluation
type: spec
status: current
domain: query
summary: Defines compilation and evaluation of one track into a scalar format-expression string.
---
# Format expression evaluation

## Scope

This specification defines how a parsed format expression becomes a `FormatPlan` and how that plan produces one string from one `library::TrackView`.
The exact grammar and supported fields belong to the [format language reference](../../reference/query/format-language.md).

This contract does not define `TrackPresentationSpec`, projected rows, UI columns, export paths, filename safety, or collision handling.

## Code boundary

This contract belongs to the **core libraries** layer in the [system architecture](../../architecture/system-overview.md) and is refined by the [track expression architecture](../../architecture/track-expression.md).
The compiler and evaluator are public under `include/ao/query/` and implemented under `lib/query/`; the current CLI consumer reads core tracks and prints the resulting strings.

## Terminology

- **Format plan** is an ordered runtime-only sequence of append-literal and append-field instructions.
- **Format binding** resolves one plan's owned dictionary symbols for one bounded evaluation batch.
- **Scalar field** is a field with one string, numeric, codec, or custom value per track.
- **Missing value** is a supported scalar field whose stored value is absent or its numeric sentinel is zero.

## Invariants

- A successful plan produces a string, never a predicate or presentation spec.
- Evaluation appends instructions from left to right without implicit separators.
- Constants append their canonical or literal text.
- Missing supported fields append an empty string.
- A plan owns custom-key symbol text and captures no dictionary or library pointer.
- Dictionary fields and custom keys are resolved through an explicit bounded dictionary context/binding.
- A plan reports the union of hot and cold track data it requires.
- Evaluation does not mutate track, dictionary, library, runtime, or frontend state.

## Compilation

`compileFormat(ast)` and `FormatCompiler::compile(ast)` return `Result<FormatPlan>` without reading or mutating a dictionary.
Compilation flattens grouping and concatenation into ordered append instructions and deduplicates repeated literal storage without changing output order.

Custom key names are retained as deduplicated plan-owned symbols.
Dictionary-backed fields mark the plan as requiring a dictionary context for evaluation.

The plan access profile is:

- `NoTrackData` when every instruction is literal;
- `HotOnly` when every field comes from hot track data;
- `ColdOnly` when every field comes from cold track data;
- `HotAndCold` when both tiers are needed.

## Evaluation

For a dictionary-using plan, `FormatBinding(plan, context)` resolves all custom-key symbols under one committed dictionary lock and retains the context for id-to-text reads.
Callers normally create one binding per output batch and reuse it for the tracks in that batch.
A later binding can resolve a custom key introduced by a newer committed dictionary generation without recompiling the plan.
When the batch evaluates transaction-backed track views, the caller opens the read transaction before creating the binding so the binding cannot be older than the evaluated storage snapshot.

Supplying every tier required by the plan is a caller precondition of every `FormatEvaluator::evaluate()` overload.
The evaluator enforces that contract before clearing caller-owned output or appending any instruction.

Otherwise, it appends each instruction:

- literal instructions append the indexed literal when the index is valid;
- dictionary fields append resolved text or empty text when unresolved;
- title and custom fields append their stored text, while an unresolved or absent custom key appends empty text;
- numeric fields append decimal text or empty text for zero;
- codec appends its canonical name or empty text for `UNKNOWN`.

A missing value contributes no characters, but adjacent literal separators remain.
For example, an absent album artist in `"[" + $albumArtist + "]"` produces `[]`.

## Failure and cancellation

Invalid subset shapes, unknown fields, and non-scalar fields return `Error::Code::FormatRejected` from the public compile boundary.
Private compiler recursion may use an internal exception, but no exception escapes for user input.

Plans with no dictionary access may use the context-free evaluation overloads.
Supplying an explicit `DictionaryReadContext` or `FormatBinding` is a precondition for a plan whose `requiresDictionary` flag is true.

Evaluation is synchronous and non-throwing for ordinary missing track values.
It has no cancellation point.
Malformed internal instruction indices are ignored defensively rather than exposing user input as memory access.
Evaluation has no insufficient-data result: a missing required hot/cold tier is a caller contract failure, while an ordinary missing value inside a present tier still contributes empty text.

## Persistence and versioning

`FormatPlan`, instructions, opcodes, literal indexes, and field ids are in-memory implementation details.
The current CLI accepts expression text for one invocation and does not persist the plan.

Scripts may retain expression text, so changes to accepted syntax and output semantics remain user-facing compatibility changes and require reference and CLI test updates.

## Frontend observations

The current consumer is plain CLI output from `track show --format`.
Using `--format` with JSON or YAML output is rejected by the CLI command boundary.
Each selected track that can be read produces one output line.

Interactive GTK and TUI track-list organization uses the separate [track-list presentation](../presentation/track-presentation.md) contract.

## Implementation map

- [`FormatExpression.h`](../../../include/ao/query/FormatExpression.h) defines plan, compiler, and evaluator values.
- [`FormatExpression.cpp`](../../../lib/query/FormatExpression.cpp) owns compilation and evaluation.
- [`TrackCommand.cpp`](../../../app/cli/TrackCommand.cpp) owns the current CLI adaptation.

## Test map

- [`FormatExpressionTest.cpp`](../../../test/unit/query/FormatExpressionTest.cpp) proves compilation, output, missing values, access profiles, failures, and field coverage.
- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) proves plain-output integration and structured-output rejection.

## Related documents

- [Track expression architecture](../../architecture/track-expression.md)
- [Format language](../../reference/query/format-language.md)
- [Predicate evaluation](predicate-evaluation.md)
- [Track-list presentation](../presentation/track-presentation.md)
