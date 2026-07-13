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
- **Scalar field** is a field with one string, numeric, codec, or custom value per track.
- **Missing value** is a supported scalar field whose stored value is absent or its numeric sentinel is zero.

## Invariants

- A successful plan produces a string, never a predicate or presentation spec.
- Evaluation appends instructions from left to right without implicit separators.
- Constants append their canonical or literal text.
- Missing supported fields append an empty string.
- Dictionary fields and custom keys are resolved through the dictionary captured by the plan.
- A plan reports the union of hot and cold track data it requires.
- Evaluation does not mutate track, dictionary, library, runtime, or frontend state.

## Compilation

`compileFormat(ast, dictionary)` and `FormatCompiler::compile(ast)` return `Result<FormatPlan>`.
Compilation flattens grouping and concatenation into ordered append instructions and deduplicates repeated literal storage without changing output order.

Dictionary-backed fields require a non-null `DictionaryStore`.
Custom key names are interned during compilation and retained as dictionary ids in field instructions.
Literal-only and non-dictionary plans may compile without a dictionary.

The plan access profile is:

- `NoTrackData` when every instruction is literal;
- `HotOnly` when every field comes from hot track data;
- `ColdOnly` when every field comes from cold track data;
- `HotAndCold` when both tiers are needed.

## Evaluation

`FormatEvaluator::evaluate(plan, track)` first verifies that the supplied `TrackView` contains every tier required by the plan.
If a required tier is missing, evaluation returns an empty result for the entire expression rather than partially formatting available fields.

Otherwise, it appends each instruction:

- literal instructions append the indexed literal when the index is valid;
- dictionary fields append resolved text or empty text when unresolved;
- title and custom fields append their stored text;
- numeric fields append decimal text or empty text for zero;
- codec appends its canonical name or empty text for `UNKNOWN`.

A missing value contributes no characters, but adjacent literal separators remain.
For example, an absent album artist in `"[" + $albumArtist + "]"` produces `[]`.

## Failure and cancellation

Invalid subset shapes, unknown fields, non-scalar fields, and missing dictionary requirements return `Error::Code::FormatRejected` from the public compile boundary.
Private compiler recursion may use an internal exception, but no exception escapes for user input.

Evaluation is synchronous and non-throwing for ordinary missing track values.
It has no cancellation point.
Malformed internal instruction indices are ignored defensively rather than exposing user input as memory access.

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
