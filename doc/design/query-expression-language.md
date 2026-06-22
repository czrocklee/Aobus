# Query Expression Language

Aobus query expressions are used by smart lists, the track filter UI, and the CLI track filter.
They describe predicates over one track at a time. Empty smart-list expressions are treated as
`true` by the runtime.

This document describes the expression language implemented by `ao::query::parse()` and the
current execution contract implemented by `QueryCompiler`/`PlanEvaluator`.

## Grammar

The grammar below is EBNF-style. It intentionally documents the user-facing shape rather than
Lexy implementation details.

```ebnf
expression          ::= or-expression ;

or-expression       ::= and-expression (("or" | "||") and-expression)* ;
and-expression      ::= relational-expression (("and" | "&&") relational-expression)* ;
relational-expression
                    ::= add-expression (("=" | "!=" | "<=" | ">=" | "~" | "<" | ">" | "in")
                        add-expression)? ;
add-expression      ::= unary-expression (("+" | adjacency) unary-expression)* ;
unary-expression    ::= ("not" | "!") unary-expression | atom ;
atom                ::= "(" expression ")" | existence-atom | variable | list | range | constant ;
existence-atom      ::= variable "?" ;

variable            ::= system-variable | user-variable ;
system-variable     ::= ("$" | "@") system-identifier ;
user-variable       ::= ("#" | "%") user-variable-name ;

system-identifier   ::= system-start system-char* ;
system-start        ::= ASCII letter | "_" ;
system-char         ::= ASCII letter | ASCII digit | "_" ;

user-variable-name  ::= bare-user-variable-name
                      | quoted-user-variable-name
                      | bracketed-quoted-user-variable-name ;
bare-user-variable-name
                    ::= (ASCII letter | ASCII digit | "_")+ ;
quoted-user-variable-name
                    ::= '"' quoted-user-variable-char+ '"' ;
bracketed-quoted-user-variable-name
                    ::= "[" quoted-user-variable-name "]" ;
quoted-user-variable-char
                    ::= any non-control Unicode character except '"' and "\\"
                      | '\\"'
                      | '\\\\'
                      | '\\''
                      | '\\n'
                      | '\\t'
                      | '\\r' ;

constant            ::= boolean | unit-number | integer | string ;
list                ::= "[" constant ("," constant)* "]" ;
range               ::= constant ".." constant ;
boolean             ::= "true" | "false" (any casing) ;
integer             ::= "-"? ASCII digit+ ;
unit-number         ::= "-"? ASCII digit+ ("." ASCII digit+)? ASCII letter+ (ASCII digit+ ASCII letter+)* ;
string              ::= bare-string | single-quoted-string | double-quoted-string ;
bare-string         ::= (ASCII letter | ASCII digit | "_")+ except "and", "or", "not", "in" (any casing) ;
single-quoted-char  ::= any non-control Unicode character except "'" and "\\"
                      | '\\"'
                      | '\\\\'
                      | '\\''
                      | '\\n'
                      | '\\t'
                      | '\\r' ;
double-quoted-char  ::= any non-control Unicode character except '"' and "\\"
                      | '\\"'
                      | '\\\\'
                      | '\\''
                      | '\\n'
                      | '\\t'
                      | '\\r' ;
single-quoted-string
                    ::= "'" single-quoted-char* "'" ;
double-quoted-string
                    ::= '"' double-quoted-char* '"' ;
```

Important implementation notes:

- Whitespace is ASCII whitespace and may appear between tokens.
- `and`, `or`, `not`, and `in` are keyword operators only when followed by an identifier boundary.
- Keyword operators (`and`, `or`, `not`, `in`) and boolean constants (`true`, `false`) are matched
  case-insensitively (`AND`, `Or`, `In`, `TRUE` all work). Because matching is case-insensitive, no
  casing of these words is usable as a bare-string value; quote it (e.g. `'AND'`) to use it as text.
- User variable names after `#` and `%` may start with a digit. System variables after `$` and `@` may not.
- `#"..."` and `%"..."` are the compact quoted user-variable-name forms. `#["..."]` and `%["..."]` are the
  explicit bracketed forms, useful when visual separation from the surrounding expression matters.
- Postfix `?` applies only to variables. `!$year?` means `!($year?)`.
- Quoted user variable names and both single- and double-quoted string constants share the same
  escape sequences: `\"`, `\\`, `\'`, `\n`, `\t` and `\r`. Invalid escapes are rejected.
- Lists are non-empty, comma-separated constant lists. Ranges are inclusive `lower..upper` pairs.
  Lists and ranges are executable only as the right operand of `in`.
- The shared expression parser accepts `+` and adjacent atoms for format expressions, so those forms
  have a defined precedence here. Query execution rejects concatenation because a string-producing
  expression is not a predicate. Use concatenation through the format expression contract instead.

## Operators

Operator precedence from tightest to loosest:

| Precedence | Operators | Meaning |
| --- | --- | --- |
| 1 | `?` | Field existence test |
| 2 | `not`, `!` | Boolean negation |
| 3 | `+`, adjacency | String concatenation for format expressions; parsed by the shared parser but rejected by query execution |
| 4 | `=`, `!=`, `<`, `<=`, `>`, `>=`, `~`, `in` | Comparison; `~` is substring match, `in` tests equality against a list or inclusion in a range |
| 5 | `and`, `&&` | Boolean conjunction |
| 6 | `or`, `||` | Boolean disjunction |

Use parentheses to make grouping explicit when mixing `and` and `or`.

## Variables

| Prefix | Domain | Name rule | Examples |
| --- | --- | --- | --- |
| `$` | Metadata fields | System identifier | `$title`, `$artist`, `$year` |
| `@` | Technical properties | System identifier | `@duration`, `@bitrate`, `@codec` |
| `#` | Tags | Bare or quoted user variable name | `#rock`, `#123`, `#"90s Rock"` |
| `%` | Custom metadata | Bare or quoted user variable name | `%isrc`, `%123`, `%"Replay Gain"` |

### Metadata Fields

| Field | Shortcuts |
| --- | --- |
| `$title` | `$t` |
| `$artist` | `$a` |
| `$album` | `$al` |
| `$albumArtist` | `$aa` |
| `$genre` | `$g` |
| `$composer` | `$c` |
| `$work` | `$w` |
| `$year` | `$y` |
| `$trackNumber` | `$tn` |
| `$trackTotal` | `$tt` |
| `$discNumber` | `$dn` |
| `$discTotal` | `$td` |
| `$coverArt` | `$ca` |

### Technical Properties

| Field | Shortcuts |
| --- | --- |
| `@duration` | `@l` |
| `@bitrate` | `@br` |
| `@sampleRate` | `@sr` |
| `@channels` | none |
| `@bitDepth` | `@bd` |
| `@codec` | none |

`@codec` accepts `UNKNOWN`, `FLAC`, `ALAC`, `AAC`, and `MP3` case-insensitively.

### Tags

A standalone tag variable is a membership test. For example, `#rock` means "the track has the
`rock` tag". Numeric and quoted tag names work the same way:

```text
#123
#"90s Rock"
#"你说得对"
```

`#rock?` is accepted as an explicit spelling of the same membership test.

### Custom Metadata

Custom metadata variables load the value for a user-defined key and are normally used with a
comparison:

```text
%isrc = "US-RC1-12-00001"
%"Replay Gain" = "-7.4 dB"
%123 = "user value"
```

## Existence Tests

Use postfix `?` to test whether a field exists:

```text
$albumArtist?
!$trackNumber?
@duration?
%rating?
#favorite?
```

Non-tag variables are not predicates by themselves. They must be compared, used as the left operand
of `in`, or tested explicitly with `?`. For example, `$year = 1990`, `$year in 1990..1999`, and
`$year?` are valid; `$year`, `@duration`, `%rating`, and `not $year` are rejected. Bare tags remain
valid membership predicates, so `#favorite` and `!#favorite` continue to work.

Existence semantics:

| Field kind | Exists when |
| --- | --- |
| String metadata (`$title`) | string is non-empty |
| Dictionary metadata (`$artist`, `$album`, `$albumArtist`, `$genre`, `$composer`, `$work`) | id is not invalid |
| Numeric metadata (`$year`, `$trackNumber`, `$trackTotal`, `$discNumber`, `$discTotal`) | value is greater than zero |
| Cover art (`$coverArt`) | primary resource id is valid |
| Numeric properties (`@duration`, `@bitrate`, `@sampleRate`, `@channels`, `@bitDepth`) | value is greater than zero |
| Codec (`@codec`) | codec is not `UNKNOWN` |
| Tag (`#favorite`) | track has the tag |
| Custom metadata (`%rating`) | key is present, even when its value is an empty string |

## Missing Values

Negate an existence test to test for a missing value. `!field?` is the canonical missing-field test;
`not field?` is equivalent.

```text
!$year?
not $composer?
!@duration?
!%rating?
```

The bare form `!$year` is rejected because its operand `$year` is a bare non-tag field; non-tag
variables are not predicates on their own. Aobus reports a hint pointing to `!$year?` so the
intended missing-value test is clear.

## Constants And Units

String constants may be bare words or quoted with single or double quotes:

```text
Bach
"The Beatles"
'Symphony No. 5'
```

Numbers are signed 64-bit integers after parsing. Unit literals are scaled during compilation based
on the left-hand field:

| Field context | Units | Meaning |
| --- | --- | --- |
| `@duration` | `ms`, `s`, `m`, `h` | milliseconds, seconds, minutes, hours |
| `@bitrate` | `k`, `m` | thousand, million |
| `@sampleRate` | `k`, `m` | thousand, million |

Unit suffixes are case-insensitive. Fractional units are accepted only when they scale to an integer
value, so `@sampleRate = 44.1k` is valid but `@duration = 1.5ms` is rejected. Duration constants may
combine multiple unit segments, such as `2m30s`.

Lists contain constants and are used with `in`:

```text
$artist in ["Bach", "Mozart"]
$year in [1990, 1991, 1992]
@duration in [3m, 4m, 5m]
%mood in ["focus", "night"]
```

`in` is equivalent to an `or` of equality tests over the same left-hand side. For example,
`$year in [1990, 1991]` means `($year = 1990) or ($year = 1991)`.
Lists may mix constant kinds; each element is compiled as its own equality comparison against the
left-hand field, matching ordinary `=` semantics.

Ranges are inclusive and are also used with `in`:

```text
$year in 1990..1999
@duration in 2m30s..5m
```

`in` with a range is equivalent to closed bounds over the same left-hand side. For example,
`$year in 1990..1999` means `($year >= 1990) and ($year <= 1999)`.

Ordered comparisons — `<`, `<=`, `>`, `>=`, and ranges — compare the left-hand field's value.
Dictionary-backed metadata fields (`$artist`, `$album`, `$genre`, `$albumArtist`, `$composer`,
`$work`) store interned IDs whose order reflects insertion rather than text, so an ordered comparison
resolves the ID back to its string and compares lexicographically (the same resolution `~` uses).
The operand must therefore be a string: `$artist in Bach..Mozart` is valid, but `$artist in 1..5`
is rejected. Equality and `in` lists over these fields still match by ID, which is both correct and
cheaper. Plain string fields such as `$title` already compare lexicographically.

## Quick Filter Expansion

The track quick filter accepts either a full expression or plain search terms. Plain terms are
expanded outside the query parser into an `and` of broad text predicates. Each term searches common
metadata fields and tag membership:

```text
beatles
```

becomes conceptually:

```text
($title ~ "beatles" or $artist ~ "beatles" or $album ~ "beatles" or
 $albumArtist ~ "beatles" or $genre ~ "beatles" or $composer ~ "beatles" or
 $work ~ "beatles" or #beatles)
```

Multiple plain terms are combined with `and`.

## Examples

| Expression | Meaning |
| --- | --- |
| `true` | Match every track. |
| `false` | Match no tracks. |
| `$artist = Bach` | Artist equals `Bach`. |
| `$artist ~ Bach` | Artist contains `Bach`. |
| `$title ~ "Goldberg Variations"` | Title contains a phrase with a space. |
| `$album = 'Kind of Blue'` | Album equals `Kind of Blue`. |
| `$year >= 2020` | Year is 2020 or later. |
| `$trackNumber = 1 and $discNumber = 2` | First track on disc 2. |
| `$genre = Classical or $genre = Jazz` | Genre is Classical or Jazz. |
| `$genre in [Classical, Jazz]` | Genre is Classical or Jazz using list membership. |
| `$artist in ["Bach", "Mozart", "Debussy"]` | Artist equals one of the listed names. |
| `$year in [1989, 1990, 1991]` | Year equals one of the listed years. |
| `$year in 1990..1999` | Year is between 1990 and 1999, inclusive. |
| `$artist in Bach..Mozart` | Artist name falls between Bach and Mozart, inclusive (lexicographic). |
| `($genre = Classical or $genre = Jazz) and @duration > 3m` | Parenthesized genre choice plus duration. |
| `$albumArtist?` | Album artist field is present. |
| `!$composer?` | Composer field is missing. |
| `!#skip` | Track does not have the `skip` tag. |
| `#favorite` | Track has the `favorite` tag. |
| `#favorite?` | Explicit existence spelling for the same tag membership test. |
| `#123` | Track has the numeric tag `123`. |
| `#"90s Rock"` | Track has the tag `90s Rock`. |
| `#["90s Rock"]` | Explicit bracketed spelling for the same tag. |
| `#"你说得对"` | Track has a Unicode tag. |
| `%isrc = "US-RC1-12-00001"` | Custom key `isrc` has the given value. |
| `%"Replay Gain" = "-7.4 dB"` | Custom key with a space in its name. |
| `%rating?` | Custom key `rating` is present, even if its value is empty. |
| `%123 = "catalogue"` | Numeric custom key. |
| `%mood in ["focus", "night"]` | Custom mood equals one of the listed values. |
| `@duration >= 3m and @duration < 10m` | Duration is at least 3 minutes and under 10 minutes. |
| `@duration in [3m, 4m, 5m]` | Duration equals one of the listed unit values. |
| `@duration in 2m30s..5m` | Duration is between 2 minutes 30 seconds and 5 minutes, inclusive. |
| `@bitrate >= 256k` | Bitrate is at least 256000. |
| `@sampleRate = 44.1k` | Sample rate equals 44100. |
| `@channels = 2 and @bitDepth >= 16` | Stereo, 16-bit or better. |
| `@codec = FLAC` | Codec is FLAC; codec names are case-insensitive. |
| `$coverArt > 0` | Track has a primary cover-art resource id. |
| `#lossless and (@codec = FLAC or @codec = ALAC)` | Lossless tag plus a lossless codec. |
| `$work ~ "Cello Suite" and $composer = Bach` | Classical work search. |
| `$albumArtist = "Various Artists" and $year < 1990` | Older compilation albums. |
| `%"Mood" ~ "night" and !#skip` | Custom mood contains `night` and track is not skipped. |

## Error Reporting

The public query entry points are non-throwing. They return `ao::Result<T>`
(`std::expected<T, ao::Error>`) and report failure as an engaged
`Error{Code::FormatRejected, message}` rather than throwing:

| Function | Header | Result |
| --- | --- | --- |
| `parse(expr)` | `ao/query/Parser.h` | `Result<Expression>` — `FormatRejected` on a syntax error. |
| `compileQuery(expr, dict)` | `ao/query/QueryCompiler.h` | `Result<ExecutionPlan>` — `FormatRejected` when `expr` is not a valid boolean predicate. |
| `compileFormat(expr, dict)` | `ao/query/FormatExpression.h` | `Result<FormatPlan>` — `FormatRejected` when `expr` is not a valid format expression. |

`QueryCompiler::compile()` and `FormatCompiler::compile()` follow the same
`Result` contract as the free-function entry points. `resolveVariableField()`
returns `Result<Field>` when callers need a diagnostic for an unknown field;
`Error::message` carries a human-readable explanation suitable for surfacing to
the user. Completion and other presence-only probes use
`tryResolveVariableField()`, which returns `std::optional<Field>` and treats an
unknown field as `std::nullopt`. Callers chain the stages and propagate the
first `Error`:

- The smart-list source (`app/runtime/source/SmartListSource.cpp`) stages the
  message into the list's error state so the UI can show why a filter was
  rejected.
- The CLI `track` command prints `filter error: <message>` to stderr.

`parse` discards lexer position (the parser uses `lexy::noop`), so a syntax
error yields a single `FormatRejected` describing the whole expression rather
than an offset. The compile stages produce the more specific messages — the
rows in the table below correspond to those errors. Completion treats a failed
`parse` as "not yet a complete predicate" (`Completion.cpp`) instead of
catching an exception, and field resolution uses the non-throwing
`tryResolveVariableField` (`ao/query/Field.h`), which returns `std::nullopt`
for unknown fields.

Internally the recursive compilers still signal semantic errors with
exceptions; `compileQuery`/`compileFormat` are thin boundaries that translate
those into `Error` so no exception escapes the public API.

## Invalid Or Unsupported Examples

| Expression | Why it fails |
| --- | --- |
| `$123 = x` | System variable names must start with an ASCII letter or `_`. |
| `@123 = 1` | Technical property names are system identifiers. |
| `#""` | Quoted tag names must be non-empty. |
| `%"" = x` | Quoted custom-metadata keys must be non-empty. |
| `$title = "a \\x"` | Invalid escape sequences in string constants are rejected. |
| `$artist in []` | `in` lists must be non-empty. |
| `$artist in [Bach,]` | Trailing list separators are rejected. |
| `$artist in Bach` | `in` requires a list or range right operand. |
| `[1990, 1991]` | Lists are only executable as the right operand of `in`. |
| `1990..1999` | Ranges are only executable as the right operand of `in`. |
| `$year in 1990..` | Range bounds are both required. |
| `$artist in 1..5` | Ordered comparisons over dictionary fields require string operands. |
| `$year` | Non-tag fields must use `?`, `in`, or an explicit comparison. |
| `@duration` | Non-tag fields are not bare predicates. |
| `%rating` | Custom metadata existence must be written as `%rating?`. |
| `!$year` | Missing-field test must include `?`; use `!$year?`. |
| `not $year` | Negated field existence must be written as `!$year?` or `not $year?`. |
| `1990?` | Postfix `?` only applies to variables. |
| `($year = 1990)?` | Postfix `?` cannot apply to grouped expressions. |
| `$title + $artist = "x"` | Parses, but execution rejects `+` today. |
| `@duration >= 10k` | `k` is not a duration unit. |
| `@bitrate >= 2k3m` | Compound unit literals are only supported for duration. |
| `@bitrate >= 3h` | `h` is not a bitrate unit. |
| `@codec = OPUS` | `OPUS` is not a supported codec name yet. |
