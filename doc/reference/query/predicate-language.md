---
id: query.predicate-language
type: reference
status: current
domain: query
summary: Defines the exact predicate expression grammar, variables, aliases, operators, literals, and units.
---
# Predicate expression language

## Scope and version

This reference defines the current text surface accepted by `ao::query::parse()` and the predicate subset accepted by `ao::query::compileQuery()`.
Predicate evaluation behavior belongs to the [predicate evaluation specification](../../spec/query/predicate-evaluation.md).

Expressions are persisted as Smart List text without a separate language version number.
The in-memory AST and execution bytecode are not persisted surfaces.
For Smart Lists, this language surface participates in the library database compatibility contract governed by `ao::library::kLibraryVersion`.
Other retained or automated expressions use the compatibility policy of their containing surface.

## Code boundary

The language belongs to the **core libraries** layer in the [system architecture](../../architecture/system-overview.md) and is refined by the [track expression architecture](../../architecture/track-expression.md).
Its public API is under `include/ao/query/`, its implementation is under `lib/query/`, and it depends on core library track and dictionary values without depending on runtime, UIModel, or frontends.

## Surface

### Grammar

The grammar below is EBNF-style and describes the user-facing token shape:

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
                      | '\\"' | '\\\\' | "\\'" | '\\n' | '\\t' | '\\r' ;

constant            ::= boolean | unit-number | integer | string ;
list                ::= "[" constant ("," constant)* "]" ;
range               ::= constant ".." constant ;
boolean             ::= "true" | "false" (any casing) ;
integer             ::= "-"? ASCII digit+ ;
unit-number         ::= "-"? ASCII digit+ ("." ASCII digit+)? ASCII letter+
                        (ASCII digit+ ASCII letter+)* ;
string              ::= bare-string | single-quoted-string | double-quoted-string ;
bare-string         ::= (ASCII letter | ASCII digit | "_")+
                        except "and", "or", "not", "in" (any casing) ;
single-quoted-string
                    ::= "'" single-quoted-char* "'" ;
double-quoted-string
                    ::= '"' double-quoted-char* '"' ;
single-quoted-char  ::= any non-control Unicode character except "'" and "\\"
                      | '\\"' | '\\\\' | "\\'" | '\\n' | '\\t' | '\\r' ;
double-quoted-char  ::= any non-control Unicode character except '"' and "\\"
                      | '\\"' | '\\\\' | "\\'" | '\\n' | '\\t' | '\\r' ;
```

ASCII whitespace may occur between tokens.
Keyword operators and booleans are case-insensitive and require an identifier boundary.
Consequently, every casing of `and`, `or`, `not`, and `in` is reserved as a bare string and must be quoted when used as data.

### Operator precedence

| Precedence | Operators | Surface role |
|---:|---|---|
| 1 | `?` | Postfix variable existence |
| 2 | `not`, `!` | Boolean negation |
| 3 | `+`, adjacency | Shared-parser string concatenation; rejected by predicate compilation |
| 4 | `=`, `!=`, `<`, `<=`, `>`, `>=`, `~`, `in` | Comparison and membership |
| 5 | `and`, `&&` | Conjunction |
| 6 | `or`, `||` | Disjunction |

### Variable domains

| Prefix | Domain | Name form | Example |
|---|---|---|---|
| `$` | Curated metadata | System identifier | `$albumArtist` |
| `@` | Technical property | System identifier | `@sampleRate` |
| `#` | Tag membership | Bare or quoted user name | `#favorite`, `#"90s Rock"` |
| `%` | Custom metadata | Bare or quoted user name | `%isrc`, `%"Replay Gain"` |

System identifiers cannot start with a digit.
Tag and custom names may start with a digit and support the compact quoted form `#"name"` or `%"name"` and the bracketed form `#["name"]` or `%["name"]`.
Quoted user-variable names must be non-empty.

### Metadata variables

| Canonical variable | Alias |
|---|---|
| `$title` | `$t` |
| `$artist` | `$a` |
| `$album` | `$al` |
| `$albumArtist` | `$aa` |
| `$composer` | `$c` |
| `$conductor` | None |
| `$ensemble` | None |
| `$work` | `$w` |
| `$movement` | `$m` |
| `$soloist` | None |
| `$genre` | `$g` |
| `$year` | `$y` |
| `$trackNumber` | `$tn` |
| `$trackTotal` | `$tt` |
| `$discNumber` | `$dn` |
| `$discTotal` | `$td` |
| `$movementNumber` | `$mn` |
| `$movementTotal` | `$mt` |
| `$coverArt` | `$ca` |

### Property variables

| Canonical variable | Alias |
|---|---|
| `@duration` | `@l` |
| `@bitrate` | `@br` |
| `@sampleRate` | `@sr` |
| `@channels` | None |
| `@bitDepth` | `@bd` |
| `@codec` | None |

`@codec` constants are `UNKNOWN`, `FLAC`, `ALAC`, `WAV`, `AAC`, and `MP3`, matched case-insensitively.

### Unit literals

| Left-field context | Units | Scale |
|---|---|---|
| `@duration` | `ms`, `s`, `m`, `h` | Milliseconds, seconds, minutes, hours |
| `@bitrate` | `k`, `m` | Thousand, million |
| `@sampleRate` | `k`, `m` | Thousand, million |

Unit suffixes are case-insensitive.
Duration may concatenate unit segments, as in `2m30s`.
A fractional literal is accepted only when scaling produces an integer, so `44.1k` is valid for sample rate while `1.5ms` is invalid for duration.

## Validation rules

- Postfix `?` applies only to a variable.
- A non-tag variable is not a predicate by itself; it requires `?`, `in`, or an explicit comparison.
- A bare tag variable is a membership predicate, and its postfix `?` form is also accepted.
- Lists are non-empty constant lists and are executable only as the right operand of `in`.
- Ranges contain two required constant bounds and are executable only as the right operand of `in`.
- Predicate compilation rejects the shared parser's `+` and adjacency concatenation nodes.
- Ordered comparisons over dictionary-backed metadata require string operands.
- Unit kinds must match the left field; compound unit segments are supported only for duration.
- Quoted strings and quoted user-variable names support `\"`, `\\`, `\'`, `\n`, `\t`, and `\r`; other escapes are rejected.
- Unknown `$` and `@` names are rejected by the shared field catalog.

## Compatibility and versioning

Smart Lists persist expression text and recompile it when materialized.
Removing or renaming a variable, alias, operator, literal form, or unit can invalidate stored lists.
Changing binding or truth semantics can change their membership without changing the list-record byte layout.

The grammar and catalog in this reference, together with the truth behavior in the [predicate evaluation specification](../../spec/query/predicate-evaluation.md), are part of the Smart List contract gated by `kLibraryVersion`.
A change that expands the storable predicate surface beyond what an existing same-version reader accepts, or that can alter whether stored text parses or compiles, what it binds to, or which tracks it matches, must increment the library version.
The old version is then rejected or explicitly migrated; the current database implementation accepts only an exact version match and has no in-place migration path.

Library YAML, playback-session state, workspace state, and CLI automation independently own the compatibility of predicate text they contain or accept.
An expression carries no nested dialect id or version; [RFC 0024](../../rfc/0024-versioned-predicate-dialect.md) rejected that additional version axis.

The core variable catalog is also used by completion, generated CLI help, and unknown-field diagnostics.
Those consumers must change with this surface rather than maintaining parallel inventories.

## Examples

```text
$artist = "Bach"
$genre in [Classical, Jazz]
$year in 1990..1999
@duration >= 2m30s and @duration < 5m
$work ~ "Cello Suite" and $composer = Bach
$conductor? and !#skip
#"90s Rock"
%"Replay Gain" = "-7.4 dB"
!$movementNumber?
```

Representative invalid forms are:

| Expression | Rejection |
|---|---|
| `$123 = x` | A system identifier cannot start with a digit. |
| `#""` | A quoted user name cannot be empty. |
| `$artist in []` | A list cannot be empty. |
| `$year in 1990..` | Both range bounds are required. |
| `$artist in 1..5` | Ordered dictionary comparison requires strings. |
| `$year` | A non-tag variable is not a predicate. |
| `!$year` | Missing-field syntax requires `!$year?`. |
| `$title + $artist = x` | Predicate compilation rejects concatenation. |
| `@duration >= 10k` | `k` is not a duration unit. |
| `@codec = OPUS` | `OPUS` is not a supported codec constant. |

## Implementation authority

- [`Parser.cpp`](../../../lib/query/Parser.cpp) defines the shared grammar.
- [`FieldCatalog.cpp`](../../../lib/query/FieldCatalog.cpp) defines canonical `$`/`@` variables and aliases.
- [`UnitDispatch.gperf`](../../../lib/query/UnitDispatch.gperf) defines unit spellings.
- [`ExecutionPlan.cpp`](../../../lib/query/ExecutionPlan.cpp) enforces the predicate subset.

## Test authority

- [`ParserTest.cpp`](../../../test/unit/query/ParserTest.cpp) and [`ExpressionTest.cpp`](../../../test/unit/query/ExpressionTest.cpp) lock grammar and AST shape.
- Execution-plan tests under [`test/unit/query/`](../../../test/unit/query/) lock variables, operators, lists, ranges, units, and invalid forms.
- [`CompletionVariableTest.cpp`](../../../test/unit/query/CompletionVariableTest.cpp) locks the shared variable and alias catalog.

## Related documents

- [Track expression architecture](../../architecture/track-expression.md)
- [Predicate evaluation](../../spec/query/predicate-evaluation.md)
- [Expression completion](../../spec/query/expression-completion.md)
- [Format language](format-language.md)
- [Track field catalog](../library/model/track-field.md)
- [Library database](../library/storage/database.md)
- [RFC 0024: versioned predicate dialect](../../rfc/0024-versioned-predicate-dialect.md), rejected in favor of containing-surface version ownership
