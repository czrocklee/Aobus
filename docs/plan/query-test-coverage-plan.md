# Query Test Coverage Plan

Date: 2026-05-03

## Goal

Maximize meaningful coverage for `lib/query` with concise, consistent tests that lock down the real supported behavior of:

- parsing
- expression normalization
- serialization
- execution-plan compilation
- runtime evaluation

This document turns the audit into an implementation-ready test plan with concrete test case names, purpose, input, expected result, and technical notes.

## Scope

Primary implementation files:

- `lib/query/Parser.cpp`
- `lib/query/Expression.cpp`
- `lib/query/Serializer.cpp`
- `lib/query/ExecutionPlan.cpp`
- `lib/query/PlanEvaluator.cpp`

Primary test files to expand or add:

- `test/unit/query/ParserTest.cpp`
- `test/unit/query/ExecutionPlanTest.cpp`
- `test/unit/query/PlanEvaluatorTest.cpp`
- `test/unit/query/ExpressionTest.cpp`
- `test/unit/query/SerializerTest.cpp`

## Current Validation Baseline

The current debug test binary at `/tmp/build/debug/test/ao_test` passes the existing query suites:

- `Parser*`: 15 cases, 59 assertions
- `ExecutionPlan*`: 44 cases, 89 assertions
- `PlanEvaluator*`: 44 cases, 96 assertions

That baseline is useful, but it does not yet prove full branch coverage or guarantee that the tested query surface matches the real supported dispatch tables.

## Key Assumptions

### Assumption 1

Unsupported query fields should fail explicitly instead of silently degrading.

Recommended behavior:

- unknown metadata names should throw during compile
- unknown property names should throw during compile

Reason: `variableTypeToField()` currently falls back to `Field::TagBloom` when a name is not found, which makes tests pass without validating the intended field mapping.

### Assumption 2

`Operator::Add` should not silently compile to `OpCode::Nop`.

Recommended behavior:

- reject `Add` during compile unless real end-to-end semantics are implemented

Reason: the parser and serializer support `Add`, but the compiler currently maps unsupported operators to `OpCode::Nop`, and the evaluator ignores `Nop`.

### Assumption 3

The query surface should be defined by the dispatch tables until the product surface changes.

Current mapped fields come from:

- `lib/query/MetadataDispatch.gperf`
- `lib/query/PropertyDispatch.gperf`
- `lib/query/UnitDispatch.gperf`

## Recommended Test Structure

### Add `test/unit/query/ExpressionTest.cpp`

Use this for direct AST normalization tests that cannot be expressed clearly through the parser because parsing already normalizes.

### Add `test/unit/query/SerializerTest.cpp`

Use this for direct serializer coverage and parser/serializer round-trip assertions.

### Expand `test/unit/query/ParserTest.cpp`

Keep the current canonicalization helper and extend it with precedence, keyword-boundary, and invalid-input coverage.

### Expand `test/unit/query/ExecutionPlanTest.cpp`

Use table-driven tests for dispatch coverage, unit scaling, bloom-mask compilation, and string-constant behavior.

### Refactor `test/unit/query/PlanEvaluatorTest.cpp`

Replace the current long positional test track constructor with a declarative `TrackSpec`-style fixture.

Recommended fixture shape:

```cpp
struct TrackSpec final
{
  std::string title = "Test Title";
  std::string artist = "Test Artist";
  std::string album = "Test Album";
  std::string albumArtist = {};
  std::string composer = {};
  std::string work = {};
  std::string uri = "/path/to/track.flac";
  std::uint16_t year = 2020;
  std::uint16_t trackNumber = 1;
  std::uint16_t totalTracks = 0;
  std::uint16_t discNumber = 0;
  std::uint16_t totalDiscs = 0;
  std::uint32_t durationMs = 180000;
  std::uint32_t bitrate = 320000;
  std::uint32_t sampleRate = 44100;
  std::uint8_t channels = 2;
  std::uint8_t bitDepth = 16;
  std::uint32_t coverArtId = 0;
  std::uint8_t rating = 0;
  std::uint16_t codecId = 0;
  std::uint32_t artistId = 0;
  std::uint32_t albumId = 0;
  std::uint32_t genreId = 0;
  std::uint32_t albumArtistId = 0;
  std::uint32_t composerId = 0;
  std::uint32_t workId = 0;
  std::vector<std::uint32_t> tagIds;
  std::vector<std::pair<std::string, std::string>> customPairs;
};
```

## Phase 1: Lock Behavior Decisions First

These tests should be written first because they define the real contract for the rest of the suite.

### Test Case: `ExecutionPlan - Unknown Metadata Field Throws`

- Purpose: make unsupported metadata names fail explicitly.
- Input: `$uri = "x"`.
- Expected: `QueryCompiler::compile()` throws `ao::Exception`.
- Technical details: `$uri` is not present in `MetadataDispatch.gperf`. Today this path can silently degrade through `Field::TagBloom`.

### Test Case: `ExecutionPlan - Unknown Property Field Throws`

- Purpose: make unsupported property names fail explicitly.
- Input: `@tagCount > 0`.
- Expected: `QueryCompiler::compile()` throws `ao::Exception`.
- Technical details: `@tagCount` is not present in `PropertyDispatch.gperf`. The current test using it should be replaced or the field should be added to the product surface first.

### Test Case: `ExecutionPlan - Add Operator Is Rejected`

- Purpose: prevent parser-only `Add` support from compiling into an ignored opcode.
- Input: `$title + $artist`.
- Expected: `QueryCompiler::compile()` throws `ao::Exception`.
- Technical details: `toOpCode()` currently falls through to `OpCode::Nop` for `Operator::Add`.

### Test Case: `ExecutionPlan - Supported Query Surface Snapshot`

- Purpose: pin the public query surface to the current dispatch tables.
- Input: one query for every mapped metadata/property token plus representative unmapped tokens.
- Expected: mapped names compile to the expected `Field`, unmapped names throw.
- Technical details: keep this table-driven so future dispatch changes require a single, obvious test update.

## Phase 2: Expression Normalization Tests

Target file: `lib/query/Expression.cpp`

### Test Case: `Expression - Normalize Collapses Binary Node Without Operation`

- Purpose: cover the branch where a `BinaryExpression` has no `operation`.
- Input: a hand-built `BinaryExpression` whose `operand` is `$title` and whose `operation` is `std::nullopt`.
- Expected: the root expression becomes just `$title` after `normalize()`.
- Technical details: build the AST manually and compare a canonicalized form before and after normalization.

### Test Case: `Expression - Normalize Leaves Constant Unchanged`

- Purpose: cover the constant visitor no-op branch.
- Input: a `ConstantExpression` containing `true`.
- Expected: the expression remains unchanged.
- Technical details: direct AST comparison is enough.

### Test Case: `Expression - Normalize Leaves Variable Unchanged`

- Purpose: cover the variable visitor no-op branch.
- Input: a `VariableExpression` for `$artist`.
- Expected: the expression remains unchanged.
- Technical details: direct AST comparison is enough.

### Test Case: `Expression - Normalize Reassociates Right Nested Add Chain`

- Purpose: cover the happy path in `shiftAdd()`.
- Input: a hand-built AST equivalent to `a + (b + c)`.
- Expected: a stable left-associated shape equivalent to `(a + b) + c`.
- Technical details: use a local canonicalizer helper so the expected shape is clear and independent from parser formatting.

### Test Case: `Expression - Normalize Reassociates Four Term Add Chain`

- Purpose: prove the recursive `shiftAdd()` path, not just one swap.
- Input: a hand-built AST equivalent to `a + (b + (c + d))`.
- Expected: a fully left-associated normalized form.
- Technical details: this is the highest-value direct normalization test.

### Test Case: `Expression - Normalize Does Not Touch NonAdd Binary`

- Purpose: cover the early return when the operator is not `Add`.
- Input: a hand-built AST equivalent to `a and (b and c)`.
- Expected: no reassociation occurs.
- Technical details: child nodes can still be normalized recursively if needed.

### Test Case: `Expression - Normalize Stops When Right Operand Is Not Binary`

- Purpose: cover the branch where the right operand is not a `BinaryExpression`.
- Input: a hand-built AST equivalent to `a + 1`.
- Expected: no reassociation occurs.
- Technical details: this targets the `std::get_if<std::unique_ptr<BinaryExpression>>() == nullptr` path.

### Test Case: `Expression - Normalize Stops When Right Binary Is Not Add`

- Purpose: cover the branch where the right nested binary exists but is not `Add`.
- Input: a hand-built AST equivalent to `a + (b and c)`.
- Expected: no reassociation occurs.
- Technical details: direct branch coverage of the `rhs->operation->op != Operator::Add` check.

### Test Case: `Expression - Normalize Unary Recurses Into Operand`

- Purpose: cover recursive normalization under unary expressions.
- Input: a hand-built AST equivalent to `not (a + (b + c))`.
- Expected: the inner add chain is normalized while the unary wrapper remains intact.
- Technical details: this covers both the unary visitor and recursive normalization path.

### Test Case: `Expression - Normalize Null Unary Pointer Is Safe`

- Purpose: cover the null `UnaryExpression` pointer branch.
- Input: `Expression{std::unique_ptr<UnaryExpression>{}}`.
- Expected: no throw and no change.
- Technical details: direct construction only.

### Test Case: `Expression - Normalize Null Binary Pointer Is Safe`

- Purpose: cover the null `BinaryExpression` pointer branch.
- Input: `Expression{std::unique_ptr<BinaryExpression>{}}`.
- Expected: no throw and no change.
- Technical details: direct construction only.

## Phase 3: Serializer Tests

Target file: `lib/query/Serializer.cpp`

### Test Case: `Serializer - Serializes Metadata Variable Prefix`

- Purpose: cover the `$` variable prefix branch.
- Input: `VariableExpression{VariableType::Metadata, "artist"}`.
- Expected: `$artist`.
- Technical details: direct AST serialization.

### Test Case: `Serializer - Serializes Property Variable Prefix`

- Purpose: cover the `@` variable prefix branch.
- Input: `VariableExpression{VariableType::Property, "duration"}`.
- Expected: `@duration`.
- Technical details: direct AST serialization.

### Test Case: `Serializer - Serializes Tag Variable Prefix`

- Purpose: cover the `#` variable prefix branch.
- Input: `VariableExpression{VariableType::Tag, "rock"}`.
- Expected: `#rock`.
- Technical details: direct AST serialization.

### Test Case: `Serializer - Serializes Custom Variable Prefix`

- Purpose: cover the `%` variable prefix branch.
- Input: `VariableExpression{VariableType::Custom, "isrc"}`.
- Expected: `%isrc`.
- Technical details: direct AST serialization.

### Test Case: `Serializer - Serializes Boolean Constant`

- Purpose: cover the bool constant visitor.
- Input: `true` and `false` constants.
- Expected: `true` and `false`.
- Technical details: two assertions in one test are fine.

### Test Case: `Serializer - Serializes Integer Constant`

- Purpose: cover the integer constant visitor.
- Input: `123` and `-7` constants.
- Expected: `123` and `-7`.
- Technical details: direct AST serialization.

### Test Case: `Serializer - Serializes Unit Constant`

- Purpose: cover the unit constant visitor.
- Input: `UnitConstantExpression{"44.1k"}`.
- Expected: `44.1k`.
- Technical details: direct AST serialization.

### Test Case: `Serializer - Serializes String Constant With Quotes`

- Purpose: cover the string constant visitor.
- Input: a string constant `"Bach"`.
- Expected: `"Bach"` with double quotes.
- Technical details: serializer should always emit double-quoted strings.

### Test Case: `Serializer - Serializes Unary Not`

- Purpose: cover the unary serializer path.
- Input: a direct AST equivalent to `not $artist`.
- Expected: `not $artist`.
- Technical details: direct AST serialization.

### Test Case: `Serializer - Serializes Each Binary Operator Token`

- Purpose: cover every binary operator token emitted by `serializeBinary()`.
- Input: one AST for each of `And`, `Or`, `Less`, `LessEqual`, `Greater`, `GreaterEqual`, `Equal`, `NotEqual`, `Like`, and `Add`.
- Expected: output uses ` and `, ` or `, ` < `, ` <= `, ` > `, ` >= `, ` = `, ` != `, ` ~ `, and ` + ` respectively.
- Technical details: table-driven implementation recommended.

### Test Case: `Serializer - Parenthesizes Nested Binary Expressions`

- Purpose: cover the nested-parentheses behavior in `ParenthesisGuard`.
- Input: a nested AST equivalent to `($artist = "Bach") and ($year >= 2020)`.
- Expected: serialized output contains the expected parentheses placement for nested binary expressions.
- Technical details: this specifically exercises the `counter++ > 0` path.

### Test Case: `Serializer - Does Not Parenthesize Root Binary Expression`

- Purpose: cover the root no-parentheses path.
- Input: a single root binary expression.
- Expected: no unnecessary outermost parentheses.
- Technical details: direct AST serialization.

### Test Case: `Serializer - Ignores Null Binary Pointer`

- Purpose: cover the null binary-pointer branch.
- Input: `Expression{std::unique_ptr<BinaryExpression>{}}`.
- Expected: empty output.
- Technical details: direct branch coverage.

### Test Case: `Serializer - Ignores Null Unary Pointer`

- Purpose: cover the null unary-pointer branch.
- Input: `Expression{std::unique_ptr<UnaryExpression>{}}`.
- Expected: empty output.
- Technical details: direct branch coverage.

### Test Case: `Serializer - RoundTrip ParseSerializeParse Preserves Canonical Shape`

- Purpose: validate serializer correctness against parser canonicalization.
- Input: representative complex valid queries such as:
  - `$artist = Bach and $year >= 2020`
  - `!($year = 2020)`
  - `$title ~ "Bach" or $composer ~ "Mozart"`
  - `%isrc = "X" and @duration >= 3m`
- Expected: parsing the serialized output yields the same canonical AST shape as the original parse.
- Technical details: compare canonicalized AST strings, not raw serialized text.

## Phase 4: Parser Tests

Target file: `lib/query/Parser.cpp`

### Precedence And Grouping

#### Test Case: `Parser - And Binds Tighter Than Or`

- Purpose: lock precedence between `and` and `or`.
- Input: `$a = x or $b = y and $c = z`.
- Expected: AST equivalent to `$a = x or ($b = y and $c = z)`.
- Technical details: assert via the existing canonicalizer pattern.

#### Test Case: `Parser - Parentheses Override Precedence`

- Purpose: ensure grouping overrides default precedence.
- Input: `($a = x or $b = y) and $c = z`.
- Expected: grouped AST equivalent to `($a = x or $b = y) and $c = z`.
- Technical details: canonical AST assertion.

#### Test Case: `Parser - Add Binds Tighter Than Relational`

- Purpose: lock precedence between additive and relational expressions.
- Input: `$trackNumber + 1 = 12`.
- Expected: AST groups the add expression before equality.
- Technical details: even if `Add` is later rejected by the compiler, parser precedence should still be deterministic.

#### Test Case: `Parser - Nested Parentheses Parse Correctly`

- Purpose: cover deep grouping.
- Input: `(($artist = Bach))`.
- Expected: same canonical AST as `$artist = Bach`.
- Technical details: simple canonical equality test.

### Keyword Boundaries And Token Rules

#### Test Case: `Parser - Bareword CanContainKeywordSubstring`

- Purpose: ensure reserved keywords only match complete keywords.
- Input: `Bandroid`, `oratorio`, and `notation`.
- Expected: each parses as a string constant, not as a keyword sequence.
- Technical details: this validates the `reserve()` usage for bareword identifiers.

#### Test Case: `Parser - Variable Identifier Requires Valid Prefix And Name`

- Purpose: validate variable token rules.
- Input: `$1bad`, `$`, `@`, `#`, and `%`.
- Expected: parse throws for each invalid token.
- Technical details: compact table-driven invalid-input test.

#### Test Case: `Parser - Custom Identifier Allows Underscore And Digits`

- Purpose: lock current custom-key identifier support.
- Input: `%replaygain_track_gain_db`.
- Expected: parse succeeds to a custom variable expression.
- Technical details: canonical AST assertion.

### Invalid Input Matrix

#### Test Case: `Parser - Rejects Empty Input`

- Purpose: retain baseline empty-input coverage.
- Input: empty string.
- Expected: parse throws `ao::Exception`.
- Technical details: keep existing coverage.

#### Test Case: `Parser - Rejects Whitespace Only`

- Purpose: retain baseline whitespace-only coverage.
- Input: spaces and tabs only.
- Expected: parse throws `ao::Exception`.
- Technical details: keep existing coverage.

#### Test Case: `Parser - Rejects Dangling Binary Operator`

- Purpose: cover a missing operand after a binary operator.
- Input: `$artist = Bach and`.
- Expected: parse throws.
- Technical details: high-value invalid syntax case.

#### Test Case: `Parser - Rejects Leading Binary Operator`

- Purpose: cover a missing left operand.
- Input: `and $artist = Bach`.
- Expected: parse throws.
- Technical details: high-value invalid syntax case.

#### Test Case: `Parser - Rejects Unterminated Single Quote`

- Purpose: cover single-quoted unterminated strings.
- Input: `'Bach`.
- Expected: parse throws.
- Technical details: string lexing failure.

#### Test Case: `Parser - Rejects Unterminated Double Quote`

- Purpose: cover double-quoted unterminated strings.
- Input: `"Bach`.
- Expected: parse throws.
- Technical details: string lexing failure.

#### Test Case: `Parser - Rejects Empty Parentheses`

- Purpose: cover grouping with no operand.
- Input: `()`.
- Expected: parse throws.
- Technical details: expression grammar failure.

#### Test Case: `Parser - Rejects Missing Right Parenthesis`

- Purpose: cover unmatched grouping.
- Input: `($artist = Bach`.
- Expected: parse throws.
- Technical details: expression grammar failure.

#### Test Case: `Parser - Rejects Relational Without Right Operand`

- Purpose: cover a missing right operand after a relational operator.
- Input: `$year >=`.
- Expected: parse throws.
- Technical details: expression grammar failure.

#### Test Case: `Parser - Rejects Unary Without Operand`

- Purpose: cover `not` and `!` without an operand.
- Input: `not` and `!`.
- Expected: parse throws.
- Technical details: two assertions in one test are fine.

## Phase 5: ExecutionPlan Tests

Target file: `lib/query/ExecutionPlan.cpp`

### Dispatch Mapping Coverage

#### Test Case: `ExecutionPlan - Metadata Dispatch Maps Every Supported Name`

- Purpose: exhaustively verify metadata-name mapping.
- Input: one query for each supported metadata token:
  - `$year`
  - `$y`
  - `$trackNumber`
  - `$tn`
  - `$totalTracks`
  - `$tt`
  - `$discNumber`
  - `$dn`
  - `$totalDiscs`
  - `$td`
  - `$artist`
  - `$a`
  - `$album`
  - `$al`
  - `$genre`
  - `$g`
  - `$composer`
  - `$c`
  - `$albumArtist`
  - `$aa`
  - `$coverArt`
  - `$ca`
  - `$title`
  - `$t`
  - `$work`
  - `$w`
- Expected: the first `LoadField` instruction contains the expected `Field` value for every token.
- Technical details: implement as a single table-driven test to keep the file concise.

#### Test Case: `ExecutionPlan - Property Dispatch Maps Every Supported Name`

- Purpose: exhaustively verify property-name mapping.
- Input: one query for each supported property token:
  - `@duration`
  - `@l`
  - `@bitrate`
  - `@br`
  - `@sampleRate`
  - `@sr`
  - `@channels`
  - `@bitDepth`
  - `@bd`
- Expected: the first `LoadField` instruction contains the expected `Field` value.
- Technical details: table-driven implementation.

#### Test Case: `ExecutionPlan - Unknown Metadata Name Throws`

- Purpose: enforce explicit failure for unsupported metadata names.
- Input: `$uri = x` and `$rating = 5` unless those names are intentionally added to the query surface.
- Expected: compile throws.
- Technical details: decision-gated by product surface.

#### Test Case: `ExecutionPlan - Unknown Property Name Throws`

- Purpose: enforce explicit failure for unsupported property names.
- Input: `@tagCount > 0` and `@codecId = 1` unless those names are intentionally added to the query surface.
- Expected: compile throws.
- Technical details: decision-gated by product surface.

### Constant Compilation

#### Test Case: `ExecutionPlan - Boolean False Compiles To ConstantZero`

- Purpose: cover constant `false` compilation.
- Input: `false`.
- Expected: the plan contains a `LoadConstant` instruction with `constValue == 0`, and `matchesAll == false`.
- Technical details: this complements the existing `true` coverage.

#### Test Case: `ExecutionPlan - Reuses Identical String Constants`

- Purpose: cover `addStringConstant()` deduplication.
- Input: `$title = "Bach" or $title != "Bach"`.
- Expected: `stringConstants.size() == 1`.
- Technical details: high-value direct coverage of the dedup path.

#### Test Case: `ExecutionPlan - Stores Different String Constants Separately`

- Purpose: cover the negative dedup case.
- Input: `$title = "Bach" or $title = "Mozart"`.
- Expected: `stringConstants.size() == 2`.
- Technical details: pairs naturally with the previous test.

#### Test Case: `ExecutionPlan - DictionaryBacked Equality Resolves To NumericId`

- Purpose: make dictionary-backed equality resolution explicit.
- Input: `$artist = "Bach"` with a seeded `DictionaryStore`.
- Expected: the constant is stored numerically, not in `stringConstants`.
- Technical details: assert both the instruction payload and `stringConstants.empty()`.

#### Test Case: `ExecutionPlan - DictionaryBacked Like Keeps StringConstant`

- Purpose: prove that `LIKE` on dictionary-backed fields does not resolve the right side to IDs.
- Input: `$artist ~ "Bach"` with a seeded `DictionaryStore`.
- Expected: `stringConstants` contains `"Bach"`, and the `Like` instruction records the left field.
- Technical details: explicit coverage of `_resolveStringConstantsToIds = false`.

#### Test Case: `ExecutionPlan - NoDictionaryLeaves Metadata Equality As StringConstant`

- Purpose: cover the no-dictionary path for metadata ID fields.
- Input: `$artist = "Bach"` without a dictionary.
- Expected: the constant is stored as a string constant.
- Technical details: this complements the dictionary-backed equality case.

### Unit Literal Scaling

#### Test Case: `ExecutionPlan - Duration Supports MsSMMHUnits`

- Purpose: cover every supported duration unit suffix.
- Input:
  - `@duration >= 1ms`
  - `@duration >= 1s`
  - `@duration >= 1m`
  - `@duration >= 1h`
- Expected:
  - `1`
  - `1000`
  - `60000`
  - `3600000`
- Technical details: compact table-driven test.

#### Test Case: `ExecutionPlan - Bitrate Supports KAndMUnits`

- Purpose: cover scaled units for bitrate.
- Input:
  - `@bitrate >= 256k`
  - `@bitrate >= 2m`
- Expected:
  - `256000`
  - `2000000`
- Technical details: compact table-driven test.

#### Test Case: `ExecutionPlan - SampleRate Supports KAndMUnits`

- Purpose: cover scaled units for sample rate.
- Input:
  - `@sampleRate >= 44.1k`
  - `@sampleRate >= 1m`
- Expected:
  - `44100`
  - `1000000`
- Technical details: compact table-driven test.

#### Test Case: `ExecutionPlan - Unit Suffix Is CaseInsensitive`

- Purpose: cover the lowercase normalization path.
- Input:
  - `@bitrate >= 256K`
  - `@sampleRate >= 44.1K`
- Expected: the compiled constants match the lowercase equivalents.
- Technical details: direct coverage of `toLower()`.

#### Test Case: `ExecutionPlan - Negative Unit Literal Compiles`

- Purpose: cover the negative-value unit-scaling path.
- Input:
  - `@bitrate >= -2k`
  - `@duration >= -3s`
- Expected:
  - `-2000`
  - `-3000`
- Technical details: assert the `LoadConstant.constValue` directly.

#### Test Case: `ExecutionPlan - Unit Literal Rejects UnsupportedSuffixForField`

- Purpose: cover `unitMultiplier()` failures.
- Input:
  - `@duration >= 10k`
  - `@bitrate >= 3h`
  - `@sampleRate >= 10ms`
- Expected: compile throws `ao::Exception`.
- Technical details: compact table-driven test.

#### Test Case: `ExecutionPlan - Unit Literal Rejects MissingNumericFieldContext`

- Purpose: cover the missing field-context branch.
- Input: a top-level unit constant expression like `3m`.
- Expected: compile throws.
- Technical details: parser produces a `UnitConstantExpression`, and compile should reject it because `_lastField` has no numeric field context.

#### Test Case: `ExecutionPlan - Unit Literal Rejects MultipleDecimalPoints`

- Purpose: cover malformed decimal lexemes.
- Input: a hand-built AST using `UnitConstantExpression{"1.2.3k"}`.
- Expected: compile throws.
- Technical details: construct the AST directly if the parser will not accept the token.

#### Test Case: `ExecutionPlan - Unit Literal Rejects EmptyFraction`

- Purpose: cover malformed decimal lexemes with an empty fractional part.
- Input: a hand-built AST using `UnitConstantExpression{"1.k"}`.
- Expected: compile throws.
- Technical details: construct the AST directly.

#### Test Case: `ExecutionPlan - Unit Literal Rejects NonIntegralResolution`

- Purpose: cover the modulus rejection path when a scaled value is not integral.
- Input: a hand-built AST using `UnitConstantExpression{"1.5ms"}` under `@duration` context.
- Expected: compile throws.
- Technical details: use a hand-built AST if the parser cannot express the exact malformed case cleanly.

#### Test Case: `ExecutionPlan - Unit Literal Rejects Overflow`

- Purpose: cover the out-of-range path.
- Input: an oversized unit constant such as `999999999999999999999m` in a numeric field context.
- Expected: compile throws.
- Technical details: direct AST construction is acceptable if parsing the token is awkward.

### Bloom Mask Compilation

#### Test Case: `ExecutionPlan - Tag Bloom Mask Is Zero Without Dictionary`

- Purpose: retain the baseline no-dictionary behavior.
- Input: `#rock` without a dictionary.
- Expected: `tagBloomMask == 0`.
- Technical details: keep one simple baseline test.

#### Test Case: `ExecutionPlan - Tag Bloom Mask For SingleTagWithDictionary`

- Purpose: cover the positive bloom-mask path.
- Input: `#rock` with a seeded dictionary.
- Expected: the plan mask contains the bit for the resolved tag ID.
- Technical details: assert the exact bit using `id.value() & 31`.

#### Test Case: `ExecutionPlan - Tag Bloom Mask Ors Tags Across And`

- Purpose: cover the `And` branch in bloom-mask combination.
- Input: `#rock and #jazz` with a seeded dictionary.
- Expected: `tagBloomMask == rockBit | jazzBit`.
- Technical details: direct compile-only assertion.

#### Test Case: `ExecutionPlan - Tag Bloom Mask Intersects Tags Across Or`

- Purpose: cover the `Or` branch in bloom-mask combination.
- Input: `#rock or #jazz` with a seeded dictionary.
- Expected: `tagBloomMask == rockBit & jazzBit`, which is usually `0` for distinct IDs.
- Technical details: direct compile-only assertion.

#### Test Case: `ExecutionPlan - Tag Bloom Mask Clears Under Not`

- Purpose: cover the unary `Not` bloom-mask path.
- Input: `not #rock` with a seeded dictionary.
- Expected: `tagBloomMask == 0`.
- Technical details: this is required to prevent over-pruning.

#### Test Case: `ExecutionPlan - Tag Bloom Mask Ignores NonTagOperands`

- Purpose: ensure bloom-mask computation only tracks tag requirements.
- Input: `#rock and $year >= 2020` with a seeded dictionary.
- Expected: the plan mask contains only the `rock` bit.
- Technical details: mixed-expression correctness check.

### Access Profile Compilation

#### Test Case: `ExecutionPlan - AccessProfile For Each Supported Hot Field`

- Purpose: exhaustively cover hot-only field classification.
- Input:
  - `$title = "X"`
  - `$artist = "X"`
  - `$album = "X"`
  - `$genre = "X"`
  - `$albumArtist = "X"`
  - `$composer = "X"`
  - `$year = 2020`
  - `#rock`
- Expected: `accessProfile == AccessProfile::HotOnly` for each.
- Technical details: table-driven test.

#### Test Case: `ExecutionPlan - AccessProfile For Each Supported Cold Field`

- Purpose: exhaustively cover cold-only field classification.
- Input:
  - `$trackNumber = 1`
  - `$totalTracks = 10`
  - `$discNumber = 1`
  - `$totalDiscs = 2`
  - `$coverArt = 42`
  - `$work = "Symphony"`
  - `%isrc = "X"`
  - `@duration >= 3m`
  - `@bitrate >= 256k`
  - `@sampleRate >= 44.1k`
  - `@channels = 2`
  - `@bitDepth = 24`
- Expected: `accessProfile == AccessProfile::ColdOnly` for each.
- Technical details: table-driven test.

#### Test Case: `ExecutionPlan - AccessProfile MixedHotAndColdAcrossMetadataAndProperty`

- Purpose: cover mixed-profile compilation.
- Input: `$year >= 2020 and @duration >= 3m`.
- Expected: `accessProfile == AccessProfile::HotAndCold`.
- Technical details: keep one representative mixed case.

#### Test Case: `ExecutionPlan - AccessProfile TrueConstantDefaultsToHotOnly`

- Purpose: document the current behavior for constant-only plans.
- Input: `true`.
- Expected: `accessProfile == AccessProfile::HotOnly`.
- Technical details: direct compile assertion.

#### Test Case: `ExecutionPlan - AccessProfile FalseConstantDefaultsToHotOnly`

- Purpose: complete constant-plan profile coverage.
- Input: `false`.
- Expected: `accessProfile == AccessProfile::HotOnly`.
- Technical details: direct compile assertion.

## Phase 6: PlanEvaluator Tests

Target file: `lib/query/PlanEvaluator.cpp`

### Fixture Refactor First

Before adding the evaluator matrix below, introduce these local helpers:

- `TrackSpec` as described above.
- `makeTrack(TrackSpec const&)` to build a track from the spec.
- `makeDictionary(std::initializer_list<std::string>)` or a small dictionary bundle helper to keep the LMDB lifetime alive.
- `compileWithDict(std::string_view expr, DictionaryStore&)` to reduce repetitive setup.

### Dictionary-Backed Equality And Like Coverage

#### Test Case: `PlanEvaluator - Evaluates Album Dictionary Equality`

- Purpose: cover album equality against dictionary IDs.
- Input: `$album = "Kind of Blue"` with a seeded dictionary and matching/non-matching tracks.
- Expected: true for the matching track and false for the non-matching track.
- Technical details: seed the dictionary and set `albumId` on the fixture track.

#### Test Case: `PlanEvaluator - Evaluates Genre Dictionary Equality`

- Purpose: cover genre equality against dictionary IDs.
- Input: `$genre = "Jazz"`.
- Expected: true for the matching track and false for the non-matching track.
- Technical details: same pattern as album.

#### Test Case: `PlanEvaluator - Evaluates AlbumArtist Dictionary Equality`

- Purpose: cover album artist equality against dictionary IDs.
- Input: `$albumArtist = "Bach"`.
- Expected: true for the matching track and false for the non-matching track.
- Technical details: same pattern as album.

#### Test Case: `PlanEvaluator - Album LikeUsesDictionaryString`

- Purpose: cover `LIKE` for album dictionary-backed fields.
- Input: `$album ~ "Blue"`.
- Expected: true when the resolved album string contains `Blue`, false otherwise.
- Technical details: dictionary-backed string loading path.

#### Test Case: `PlanEvaluator - Genre LikeUsesDictionaryString`

- Purpose: cover `LIKE` for genre dictionary-backed fields.
- Input: `$genre ~ "Jazz"`.
- Expected: true when the resolved genre string contains `Jazz`, false otherwise.
- Technical details: dictionary-backed string loading path.

#### Test Case: `PlanEvaluator - AlbumArtist LikeUsesDictionaryString`

- Purpose: cover `LIKE` for album artist dictionary-backed fields.
- Input: `$albumArtist ~ "Bach"`.
- Expected: true when the resolved album artist string contains `Bach`, false otherwise.
- Technical details: dictionary-backed string loading path.

#### Test Case: `PlanEvaluator - DictionaryLikeWithMissingDictionaryIdReturnsFalse`

- Purpose: cover the `dictionaryId == 0` branch for dictionary-backed `LIKE`.
- Input: `$artist ~ "Bach"` with a track whose `artistId` is `0`.
- Expected: false.
- Technical details: direct coverage of the empty-string fallback branch.

### Custom Field Runtime Coverage

#### Test Case: `PlanEvaluator - Custom Equality MatchesStoredValue`

- Purpose: cover equality on `%custom` fields.
- Input: `%mood = "happy"` with matching and non-matching tracks.
- Expected: true for a matching stored custom value, false otherwise.
- Technical details: add custom key/value pairs via `TrackBuilder::custom().add()`.

#### Test Case: `PlanEvaluator - Custom InequalityMatchesStoredValue`

- Purpose: cover `!=` on `%custom` fields.
- Input: `%mood != "sad"`.
- Expected: true when the stored value is different, false when it matches exactly.
- Technical details: string-comparison path in `executeComparison()`.

#### Test Case: `PlanEvaluator - Custom LikeMatchesSubstring`

- Purpose: cover `LIKE` on `%custom` fields.
- Input: `%comment ~ "live"`.
- Expected: true when the stored value contains `live`, false otherwise.
- Technical details: direct coverage of `Field::Custom` string loading.

#### Test Case: `PlanEvaluator - Custom MissingKeyBehavesAsEmptyStringForEquality`

- Purpose: lock the missing-key fallback behavior.
- Input: `%isrc = ""` on a track with no such custom key.
- Expected: true if missing keys are intended to read as empty string.
- Technical details: current implementation returns empty string when the key lookup misses.

#### Test Case: `PlanEvaluator - Custom MissingKeyFailsNonEmptyEquality`

- Purpose: validate the negative case for missing custom keys.
- Input: `%isrc = "US123"` on a track with no such key.
- Expected: false.
- Technical details: same missing-key branch as above.

#### Test Case: `PlanEvaluator - Custom LexicographicLessThan`

- Purpose: cover lexicographic string comparison for custom fields.
- Input: `%mood < "z"`.
- Expected: true for values that compare less than `z`, false otherwise.
- Technical details: direct string-branch coverage in `executeComparison()`.

#### Test Case: `PlanEvaluator - Custom LexicographicGreaterEqual`

- Purpose: cover another lexicographic string comparison path.
- Input: `%mood >= "happy"`.
- Expected: true or false according to lexicographic comparison.
- Technical details: pairs well with the previous test.

### Cold Numeric Field Coverage

#### Test Case: `PlanEvaluator - Evaluates CoverArt Numeric Equality`

- Purpose: cover cold metadata numeric equality.
- Input: `$coverArt = 42`.
- Expected: true for a track with `coverArtId == 42`, false otherwise.
- Technical details: requires fixture support for `coverArtId`.

#### Test Case: `PlanEvaluator - Evaluates TrackNumber NumericComparisons`

- Purpose: cover cold metadata numeric comparisons.
- Input: representative queries such as:
  - `$trackNumber > 3`
  - `$trackNumber = 5`
  - `$trackNumber < 2`
- Expected: true or false according to the fixture values.
- Technical details: can be table-driven.

#### Test Case: `PlanEvaluator - Evaluates TotalTracks NumericComparisons`

- Purpose: cover `totalTracks` evaluation.
- Input: `$totalTracks >= 10`.
- Expected: true for `10` or more, false otherwise.
- Technical details: cold metadata numeric path.

#### Test Case: `PlanEvaluator - Evaluates DiscNumber NumericComparisons`

- Purpose: cover `discNumber` evaluation.
- Input: `$discNumber = 2`.
- Expected: true for disk number `2`, false otherwise.
- Technical details: cold metadata numeric path.

#### Test Case: `PlanEvaluator - Evaluates TotalDiscs NumericComparisons`

- Purpose: cover `totalDiscs` evaluation.
- Input: `$totalDiscs != 1`.
- Expected: true or false according to the fixture values.
- Technical details: cold metadata numeric path.

#### Test Case: `PlanEvaluator - Evaluates BitDepth NumericComparisons`

- Purpose: cover property-field bit depth evaluation.
- Input: `@bitDepth >= 24`.
- Expected: true for `24` or higher, false otherwise.
- Technical details: currently missing runtime coverage.

#### Test Case: `PlanEvaluator - Evaluates Channels NumericComparisons`

- Purpose: cover property-field channels evaluation.
- Input: `@channels = 2`.
- Expected: true for stereo tracks, false otherwise.
- Technical details: currently missing runtime coverage.

### Boolean And Logical Coverage

#### Test Case: `PlanEvaluator - FalseConstantAlwaysFails`

- Purpose: complete constant-plan runtime behavior.
- Input: `false`.
- Expected: `evaluateFull()` and `matches()` both return false for any track.
- Technical details: complements existing `true` coverage.

#### Test Case: `PlanEvaluator - DoubleNotRestoresTruthiness`

- Purpose: cover chained unary operations.
- Input: `not not ($year = 2020)`.
- Expected: true when year is `2020`, false otherwise.
- Technical details: useful direct coverage of repeated `Not` execution.

### Bloom Fast Path Runtime Coverage

#### Test Case: `PlanEvaluator - MatchesFastRejectsWhenTrackBloomMissingRequiredBit`

- Purpose: cover the real fast-reject path with a non-zero mask.
- Input: compile `#rock` with a seeded dictionary, then evaluate a track whose bloom does not include the required bit.
- Expected: `matches()` returns false.
- Technical details: this must be dictionary-backed so `tagBloomMask` is non-zero.

#### Test Case: `PlanEvaluator - MatchesPassesBloomAndMatchesTag`

- Purpose: cover the real pass path with a non-zero mask.
- Input: compile `#rock` with a seeded dictionary and evaluate a track that has both the bloom bit and the actual tag ID.
- Expected: `matches()` returns true.
- Technical details: assert both the bloom bit and the actual tag membership.

#### Test Case: `PlanEvaluator - MatchesPassesBloomButFailsFullEvaluationWhenTagArrayMissingId`

- Purpose: prove bloom false positives do not cause false query matches.
- Input: a track whose bloom bit is set for the tag but whose actual tag array does not contain the tag ID.
- Expected: `matches()` returns false.
- Technical details: high-value correctness test for the optimization boundary.

#### Test Case: `PlanEvaluator - MatchesAndCombinesRequiredTagBits`

- Purpose: validate runtime behavior for `and`-combined tag bloom requirements.
- Input: compile `#rock and #jazz` with a seeded dictionary and evaluate a track that only has one of the two tags.
- Expected: `matches()` returns false.
- Technical details: runtime complement to the compile-time mask test.

#### Test Case: `PlanEvaluator - MatchesOrDoesNotOverPruneDistinctTags`

- Purpose: validate runtime behavior for `or`-combined tag expressions.
- Input: compile `#rock or #jazz` with a seeded dictionary and evaluate a track that has only `#jazz`.
- Expected: `matches()` returns true.
- Technical details: protects the mask-intersection logic from over-pruning.

#### Test Case: `PlanEvaluator - MatchesNotTagSkipsBloomOptimization`

- Purpose: verify that `not #rock` does not use bloom fast-reject incorrectly.
- Input: compile `not #rock` with a seeded dictionary and evaluate a track without the tag.
- Expected: `matches()` returns true by falling through to full evaluation.
- Technical details: runtime complement to the compile-time `tagBloomMask == 0` expectation.

### Track Data Availability Matrix

#### Test Case: `PlanEvaluator - MatchesHotOnlyPlanRejectsHotInvalidTrack`

- Purpose: cover missing hot-data behavior through `matches()`.
- Input: a hot-only plan such as `$year = 2020` on an invalid or empty hot view.
- Expected: `matches()` returns false.
- Technical details: complements the existing invalid-track baseline.

#### Test Case: `PlanEvaluator - MatchesHotOnlyPlanIgnoresMissingColdData`

- Purpose: verify hot-only plans can run on hot-only views.
- Input: `$year = 2020` on `hotOnlyView()`.
- Expected: `matches()` returns true when the hot data matches.
- Technical details: explicitly exercise `matches()`, not just `evaluateFull()`.

#### Test Case: `PlanEvaluator - MatchesColdOnlyPlanIgnoresMissingHotData`

- Purpose: verify cold-only plans can run on cold-only views.
- Input: `@duration >= 3m` on `coldOnlyView()`.
- Expected: `matches()` returns true when the cold data matches.
- Technical details: explicitly exercise `matches()`.

#### Test Case: `PlanEvaluator - MatchesMixedPlanRejectsHotOnlyView`

- Purpose: verify mixed plans fail when cold data is missing.
- Input: `$year = 2020 and @duration >= 3m` on `hotOnlyView()`.
- Expected: `matches()` returns false.
- Technical details: symmetry with existing `evaluateFull()` coverage.

#### Test Case: `PlanEvaluator - MatchesMixedPlanRejectsColdOnlyView`

- Purpose: verify mixed plans fail when hot data is missing.
- Input: `$year = 2020 and @duration >= 3m` on `coldOnlyView()`.
- Expected: `matches()` returns false.
- Technical details: symmetry with existing `evaluateFull()` coverage.

#### Test Case: `PlanEvaluator - EvaluateFullHotOnlyPlanIgnoresMissingColdData`

- Purpose: make `evaluateFull()` symmetry explicit for hot-only plans.
- Input: `$year = 2020` on `hotOnlyView()`.
- Expected: `evaluateFull()` returns true when the hot data matches.
- Technical details: explicit symmetry test.

#### Test Case: `PlanEvaluator - EvaluateFullColdOnlyPlanIgnoresMissingHotData`

- Purpose: make `evaluateFull()` symmetry explicit for cold-only plans.
- Input: `@duration >= 3m` on `coldOnlyView()`.
- Expected: `evaluateFull()` returns true when the cold data matches.
- Technical details: existing behavior is already partly covered, but keep this explicit if the suite is reorganized.

## Phase 7: Replace Or Rewrite Misleading Existing Tests

### Existing Test: `ExecutionPlan - LIKE operator works for Uri`

- Problem: `$uri` is not currently mapped in `MetadataDispatch.gperf`.
- Action: replace this with `ExecutionPlan - Unknown Metadata Field Throws`, or add `uri` to the supported query surface first and then rewrite the test around real field mapping.

### Existing Test: `PlanEvaluator - TagCount Field`

- Problem: `@tagCount` is not currently mapped in `PropertyDispatch.gperf`.
- Action: replace this with `ExecutionPlan - Unknown Property Field Throws`, or add `tagCount` to the supported query surface before keeping a runtime test.

### Existing Bloom Tests Without Dictionary Backing

- Problem: several current tests only prove `tagBloomMask == 0`, not the real optimization behavior.
- Action: keep one no-dictionary baseline, but replace the rest with real dictionary-backed bloom-mask tests.

## Recommended Execution Order

1. Add decision-gated compile tests for unknown fields and `Add`.
2. Add `ExpressionTest.cpp`.
3. Add `SerializerTest.cpp`.
4. Expand parser precedence and invalid-input coverage.
5. Refactor the evaluator fixture into a declarative `TrackSpec`.
6. Expand compile-time dispatch, unit scaling, bloom-mask, and string-dedup coverage.
7. Expand evaluator custom-field, dictionary-LIKE, cold-field, and bloom fast-path coverage.
8. Remove or rewrite misleading tests that currently pass without validating the intended field surface.

## Highest-Value First 10 Tests

If implementation needs to start with the shortest path to the biggest confidence gain, begin with these tests in order:

1. `ExecutionPlan - Unknown Metadata Field Throws`
2. `ExecutionPlan - Unknown Property Field Throws`
3. `ExecutionPlan - Add Operator Is Rejected`
4. `Expression - Normalize Reassociates Four Term Add Chain`
5. `Serializer - RoundTrip ParseSerializeParse Preserves Canonical Shape`
6. `ExecutionPlan - Reuses Identical String Constants`
7. `ExecutionPlan - Tag Bloom Mask Ors Tags Across And`
8. `PlanEvaluator - Custom Equality MatchesStoredValue`
9. `PlanEvaluator - MatchesPassesBloomButFailsFullEvaluationWhenTagArrayMissingId`
10. `PlanEvaluator - Album LikeUsesDictionaryString`

## Done Criteria

This plan is complete when:

- unsupported query names fail explicitly or are formally added to the supported surface
- `Expression.cpp` and `Serializer.cpp` both have direct tests
- dispatch coverage is exhaustive and table-driven
- unit-scaling edge cases are covered, including error paths
- evaluator coverage includes custom fields, dictionary-backed fields, cold numeric fields, and real bloom optimization behavior
- misleading existing tests are removed or rewritten so the suite only validates real supported behavior
