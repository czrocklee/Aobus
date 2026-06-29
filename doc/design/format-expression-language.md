# Format Expression Language

Format expressions generate strings from one track at a time. They are intended for filename/path
planning, export labels, and other track-derived text. They share the query parser's field syntax,
but they compile to a string-producing `FormatPlan` rather than a boolean
`ExecutionPlan`.

The first implementation is intentionally small: variables, constants, and `+` concatenation.
Query-only operations such as comparisons, `and`, `or`, `not`, `in`, ranges, lists, and postfix `?`
are rejected by the formatter compiler.

## Examples

```text
$artist + " - " + $title
$albumArtist + "/" + $year + " - " + $album
$trackNumber + "/" + $trackTotal + " " + $title
@codec + " " + @sampleRate + "Hz " + @bitDepth + "bit"
%catalog + " " + $title
```

## Grammar

The user-facing formatter subset is:

```ebnf
format-expression   ::= format-term (("+" | adjacency) format-term)* ;
format-term         ::= "(" format-expression ")" | variable | constant ;

variable            ::= system-variable | custom-variable ;
system-variable     ::= ("$" | "@") system-identifier ;
custom-variable     ::= "%" user-variable-name ;
constant            ::= boolean | unit-number | integer | string ;
```

The parser accepts the broader query expression grammar first, then `FormatCompiler` rejects
forms outside this subset. This keeps field tokenization and quoting rules consistent with query
expressions while preserving a distinct string-producing contract.

Constants follow query literal tokenization: boolean constants are matched case-insensitively and
format as `true`/`false`, while logical keywords (`and`, `or`, `not`, `in`) are reserved as bare
strings in any casing and must be quoted to produce literal text.

## Field Formatting

| Field kind | Formatted value |
| --- | --- |
| String metadata (`$title`) | raw string |
| Dictionary metadata (`$artist`, `$album`, `$albumArtist`, `$genre`, `$composer`, `$work`) | resolved dictionary text |
| Numeric metadata (`$year`, `$trackNumber`, `$trackTotal`, `$discNumber`, `$discTotal`) | decimal string, or empty when the stored value is `0` |
| Numeric properties (`@duration`, `@bitrate`, `@sampleRate`, `@channels`, `@bitDepth`) | decimal string, or empty when the stored value is `0` |
| Codec (`@codec`) | codec name, or empty when the codec is `UNKNOWN` |
| Custom metadata (`%catalog`) | stored value, or empty when the key is absent |

Tags (`#favorite`) and cover art (`$coverArt`) are not scalar string fields and are rejected by the
formatter compiler.

Missing values format as empty strings in the first implementation. Higher-level file organization
logic should add fallback/path helpers before using formatter output as a final filesystem path.

## Query Boundary

Query expressions answer whether a track matches a predicate. Format expressions answer what string
to produce for a track. The expression core can parse both shapes, but each compiler enforces
its own result type:

```text
$artist ~ Bach              # query predicate
$artist + " - " + $title    # format expression
```

As a result, `$artist + " - " + $title` is rejected by `QueryCompiler` and accepted by
`FormatCompiler`.

## Extension Points

- Add fallback helpers for missing metadata, starting with `coalesce($albumArtist, $artist,
  "Unknown Artist")`.
- Add numeric formatting helpers such as `pad($trackNumber, 2)` for filenames and disc/track labels.
- Add filesystem helpers:
  - `safe(value)` for replacing path separators, control characters, reserved names, and other
    platform-invalid filename characters.
  - `path(part...)` for joining sanitized path segments, skipping empty segments, and avoiding
    duplicate separators.
- Decide whether unit constants in formatter output should remain literal text (`3m`) or format as
  scaled numeric values in a field-aware helper.
- Add a higher-level file organization contract that consumes `FormatPlan` output and validates final
  paths for length, empty basenames, collisions, and platform rules.
- Consider extracting shared track field access helpers after formatter helper semantics are stable;
  query comparison and formatter output intentionally still interpret some fields differently.
- Consider computed string predicates later, for example `($artist + " - " + $title) ~ "Bach - Cello"`,
  only after formatter value semantics are stable.
