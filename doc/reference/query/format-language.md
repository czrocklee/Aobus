---
id: query.format-language
type: reference
status: current
domain: query
summary: Defines the exact string-producing format expression grammar and supported scalar fields.
---
# Format expression language

## Scope and version

This reference defines the current string-producing subset accepted by `ao::query::compileFormat()`.
It shares lexical parsing and field names with the [predicate language](predicate-language.md) but has a distinct result type and validation surface.
Evaluation behavior belongs to the [format evaluation specification](../../spec/query/format-evaluation.md).

The current product consumer is plain `aobus track show --format` output.
Format expressions are not track-list presentation specs, column definitions, or filesystem path templates.

## Code boundary

The language belongs to the **core libraries** layer in the [system architecture](../../architecture/system-overview.md) and is refined by the [track expression architecture](../../architecture/track-expression.md).
Its public API is `include/ao/query/FormatExpression.h`, its implementation is `lib/query/FormatExpression.cpp`, and the CLI adapter is the current application consumer.

## Surface

### Grammar

```ebnf
format-expression   ::= format-term (("+" | adjacency) format-term)* ;
format-term         ::= "(" format-expression ")" | variable | constant ;

variable            ::= system-variable | custom-variable ;
system-variable     ::= ("$" | "@") system-identifier ;
custom-variable     ::= "%" user-variable-name ;
constant            ::= boolean | unit-number | integer | string ;
```

The shared parser first accepts its expression superset.
`FormatCompiler` then accepts only variables, constants, grouping, `+`, and adjacency concatenation.

Constants use predicate-language tokenization.
Booleans format canonically as `true` or `false`; integers format in decimal; unit constants preserve their original lexeme.
Keyword strings such as `and` or `in` must be quoted.

### Supported metadata variables

| Variable | Output source |
|---|---|
| `$title` | Raw title text |
| `$artist` | Resolved dictionary text |
| `$album` | Resolved dictionary text |
| `$albumArtist` | Resolved dictionary text |
| `$composer` | Resolved dictionary text |
| `$conductor` | Resolved dictionary text |
| `$ensemble` | Resolved dictionary text |
| `$work` | Resolved dictionary text |
| `$movement` | Resolved dictionary text |
| `$soloist` | Resolved dictionary text |
| `$genre` | Resolved dictionary text |
| `$year` | Decimal value or empty for zero |
| `$trackNumber` | Decimal value or empty for zero |
| `$trackTotal` | Decimal value or empty for zero |
| `$discNumber` | Decimal value or empty for zero |
| `$discTotal` | Decimal value or empty for zero |
| `$movementNumber` | Decimal value or empty for zero |
| `$movementTotal` | Decimal value or empty for zero |

The aliases from the predicate language are accepted for the same fields.
`$coverArt` is rejected because it is not a scalar string field.

### Supported property variables

| Variable | Output source |
|---|---|
| `@duration` | Millisecond count or empty for zero |
| `@bitrate` | Bits-per-second count or empty for zero |
| `@sampleRate` | Hertz count or empty for zero |
| `@channels` | Channel count or empty for zero |
| `@bitDepth` | Bit count or empty for zero |
| `@codec` | Codec name or empty for `UNKNOWN` |

Property aliases from the predicate language are accepted.

### Custom variables

`%name`, `%"quoted name"`, and `%["quoted name"]` resolve a custom metadata key and append its stored value.
An absent key appends an empty string.

Tag variables are rejected because tag membership is not one scalar field value.

## Validation rules

- Only `+` and adjacency are accepted binary operations.
- Unary operations, comparisons, logical operations, postfix existence, lists, and ranges are rejected.
- `$coverArt` and every `#tag` variable are rejected as non-scalar fields.
- Unknown system fields are rejected through the shared query field catalog.
- Compilation is dictionary-independent; custom names are stored as plan-owned symbols.
- Dictionary-backed and custom fields require an explicit dictionary context/binding at evaluation time.
- Literal-only plans require neither a dictionary context nor track data.
- The language has no functions, fallback operator, padding helper, path joiner, or path sanitization primitive.

## Compatibility and versioning

Format plans are runtime-only and are not persisted.
Text passed through CLI scripts is nevertheless a user-facing compatibility surface.
Changes to shared variable names, aliases, literals, or quoting rules must remain coordinated with the predicate language.

Adding path-oriented helpers does not by itself make evaluator output safe as a filesystem path; any future file-organization feature requires its own collision, validation, and platform-path contract.

## Examples

```text
$artist + " - " + $title
$albumArtist + "/" + $year + " - " + $album
$trackNumber + "/" + $trackTotal + " " + $title
$conductor + " / " + $ensemble + " / " + $soloist
@codec + " " + @sampleRate + "Hz " + @bitDepth + "bit"
%catalog + " " + $title
$artist " - " $title
```

Representative invalid forms are:

| Expression | Rejection |
|---|---|
| `$artist = Bach` | Comparisons are predicate-only. |
| `$year?` | Existence is predicate-only. |
| `not $title` | Unary operators are unsupported. |
| `$year in 1990..1999` | Lists, ranges, and `in` are predicate-only. |
| `#favorite` | A tag is not a scalar string field. |
| `$coverArt` | Cover selection is not a scalar string field. |

## Implementation authority

- [`Parser.cpp`](../../../lib/query/Parser.cpp) owns the shared lexical and AST grammar.
- [`FormatExpression.cpp`](../../../lib/query/FormatExpression.cpp) owns subset validation and scalar field compilation.
- [`FieldCatalog.cpp`](../../../lib/query/FieldCatalog.cpp) owns canonical variables and aliases.

## Test authority

- [`FormatExpressionTest.cpp`](../../../test/unit/query/FormatExpressionTest.cpp) locks supported fields, concatenation, literals, missing values, pure compilation, explicit binding, access profiles, and rejected forms.
- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) locks the `track show --format` workflow.

## Related documents

- [Track expression architecture](../../architecture/track-expression.md)
- [Format evaluation](../../spec/query/format-evaluation.md)
- [Predicate language](predicate-language.md)
- [Track field catalog](../library/model/track-field.md)
