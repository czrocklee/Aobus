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
                    ::= add-expression (("=" | "!=" | "<=" | ">=" | "~" | "<" | ">") add-expression)? ;
add-expression      ::= unary-expression (("+" | adjacency) unary-expression)* ;
unary-expression    ::= ("not" | "!") unary-expression | atom ;
atom                ::= "(" expression ")" | variable | constant ;

variable            ::= system-variable | user-variable ;
system-variable     ::= ("$" | "@") system-identifier ;
user-variable       ::= ("#" | "%") user-name ;

system-identifier   ::= system-start system-char* ;
system-start        ::= ASCII letter | "_" ;
system-char         ::= ASCII letter | ASCII digit | "_" ;

user-name           ::= bare-user-name | quoted-user-name | bracketed-quoted-user-name ;
bare-user-name      ::= (ASCII letter | ASCII digit | "_")+ ;
quoted-user-name    ::= '"' quoted-user-char+ '"' ;
bracketed-quoted-user-name
                    ::= "[" quoted-user-name "]" ;
quoted-user-char    ::= any non-control Unicode character except '"' and "\"
                      | '\"'
                      | '\\' ;

constant            ::= boolean | unit-number | integer | string ;
boolean             ::= "true" | "false" ;
integer             ::= "-"? ASCII digit+ ;
unit-number         ::= "-"? ASCII digit+ ("." ASCII digit+)? ASCII letter+ ;
string              ::= bare-string | single-quoted-string | double-quoted-string ;
bare-string         ::= (ASCII letter | ASCII digit | "_")+ except "and", "or", "not" ;
single-quoted-string
                    ::= "'" any characters up to the next "'" "'" ;
double-quoted-string
                    ::= '"' any characters up to the next '"' '"' ;
```

Important implementation notes:

- Whitespace is ASCII whitespace and may appear between tokens.
- `and`, `or`, and `not` are keyword operators only when followed by an identifier boundary.
- User names after `#` and `%` may start with a digit. System variables after `$` and `@` may not.
- `#"..."` and `%"..."` are the compact quoted user-name forms. `#["..."]` and `%["..."]` are the
  explicit bracketed forms, useful when visual separation from the surrounding expression matters.
- Quoted user names support `\"` and `\\`. String constants currently do not support escape
  sequences; use the other quote character when possible.
- The parser accepts `+` and adjacent atoms as concatenation syntax, but query execution currently
  rejects `+` with `operator '+' is not yet supported in query execution`. Do not use it in smart
  lists or CLI filters yet.

## Operators

Operator precedence from tightest to loosest:

| Precedence | Operators | Meaning |
| --- | --- | --- |
| 1 | `not`, `!` | Boolean negation |
| 2 | `+`, adjacency | Parser-level concatenation, not executable yet |
| 3 | `=`, `!=`, `<`, `<=`, `>`, `>=`, `~` | Comparison; `~` is substring match |
| 4 | `and`, `&&` | Boolean conjunction |
| 5 | `or`, `||` | Boolean disjunction |

Use parentheses to make grouping explicit when mixing `and` and `or`.

## Variables

| Prefix | Domain | Name rule | Examples |
| --- | --- | --- | --- |
| `$` | Metadata fields | System identifier | `$title`, `$artist`, `$year` |
| `@` | Technical properties | System identifier | `@duration`, `@bitrate`, `@codec` |
| `#` | Tags | Bare or quoted user name | `#rock`, `#123`, `#"90s Rock"` |
| `%` | Custom metadata | Bare or quoted user name | `%isrc`, `%123`, `%"Replay Gain"` |

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

### Custom Metadata

Custom metadata variables load the value for a user-defined key and are normally used with a
comparison:

```text
%isrc = "US-RC1-12-00001"
%"Replay Gain" = "-7.4 dB"
%123 = "user value"
```

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
value, so `@sampleRate = 44.1k` is valid but `@duration = 1.5ms` is rejected.

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
| `($genre = Classical or $genre = Jazz) and @duration > 3m` | Parenthesized genre choice plus duration. |
| `not $composer` | Composer field is logically false/empty. |
| `!#skip` | Track does not have the `skip` tag. |
| `#favorite` | Track has the `favorite` tag. |
| `#123` | Track has the numeric tag `123`. |
| `#"90s Rock"` | Track has the tag `90s Rock`. |
| `#["90s Rock"]` | Explicit bracketed spelling for the same tag. |
| `#"你说得对"` | Track has a Unicode tag. |
| `%isrc = "US-RC1-12-00001"` | Custom key `isrc` has the given value. |
| `%"Replay Gain" = "-7.4 dB"` | Custom key with a space in its name. |
| `%123 = "catalogue"` | Numeric custom key. |
| `@duration >= 3m and @duration < 10m` | Duration is at least 3 minutes and under 10 minutes. |
| `@bitrate >= 256k` | Bitrate is at least 256000. |
| `@sampleRate = 44.1k` | Sample rate equals 44100. |
| `@channels = 2 and @bitDepth >= 16` | Stereo, 16-bit or better. |
| `@codec = FLAC` | Codec is FLAC; codec names are case-insensitive. |
| `$coverArt > 0` | Track has a primary cover-art resource id. |
| `#lossless and (@codec = FLAC or @codec = ALAC)` | Lossless tag plus a lossless codec. |
| `$work ~ "Cello Suite" and $composer = Bach` | Classical work search. |
| `$albumArtist = "Various Artists" and $year < 1990` | Older compilation albums. |
| `%"Mood" ~ "night" and !#skip` | Custom mood contains `night` and track is not skipped. |

## Invalid Or Unsupported Examples

| Expression | Why it fails |
| --- | --- |
| `$123 = x` | System variable names must start with an ASCII letter or `_`. |
| `@123 = 1` | Technical property names are system identifiers. |
| `#""` | Quoted user names must be non-empty. |
| `%"" = x` | Quoted custom keys must be non-empty. |
| `$title = "a \"quote\""` | String constants do not currently support escape sequences. |
| `$title + $artist = "x"` | Parses, but execution rejects `+` today. |
| `@duration >= 10k` | `k` is not a duration unit. |
| `@bitrate >= 3h` | `h` is not a bitrate unit. |
| `@codec = OPUS` | `OPUS` is not a supported codec name yet. |
