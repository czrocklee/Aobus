# Query and Metadata Completion

Aobus offers inline completion in two distinct places, served by one shared vocabulary and one
shared UI adapter:

1. **Query-expression completion** — while typing a [query expression](query-expression-language.md)
   (smart-list editor, the track quick filter), completion covers variable names, operators after a
   recognized left-hand variable, logical operators after a complete expression, and selected
   dictionary-backed values after a complete operator.
2. **Metadata-value completion** — while editing a track metadata field (track properties dialog,
   the [track detail grid](track-detail-grid.md)), the field's existing library values are offered.

## Layering

The engine is split across three layers so the grammar stays free of UI and library concerns:

- **`lib/query` (`ao/query/Completion.h`)** — pure grammar. `analyzeCompletionContext()` classifies the
  cursor as `Variable`, field `Operator`, `LogicalOperator`, or `Value` without requiring the expression
  to parse as a complete AST. It works over a token view produced by `detail::tokenizeCompletionQuery()`
  rather than re-deriving lexing by hand. `queryCompletionTokenAtCursor()` remains a variable-only
  compatibility wrapper. `queryVariableCompletionSpecs()` / `completeQueryVariable()` resolve the
  *authoritative* catalog of `$`/`@` variables, including aliases and `$coverArt` that exist only on the
  query side. `resolveVariableField()` uses the same catalog, so parser/compiler field resolution and
  completion names have one shared source.

### Lexical source of truth

The completion tokenizer (`detail/CompletionTokenizer.cpp`) reuses the parser's own lexical rules from
`detail/Lexical.h`: variable sigils, quoted/bracketed names, literals, and operator spellings are each
defined once and consumed by both the parser grammar (`dsl::p<…>`, `dsl::op<…>`) and the tokenizer (via
the `AsToken<…>` capturing wrapper). A new sigil or operator spelling therefore cannot drift between the
parser and completion.

The one exception is the **partial-tail** boundary — the incomplete token under the cursor (an open
quote, a lone sigil, an unterminated `["…"]`). The parser cannot lex incomplete input, so this has no
shared rule and is detected by a small hand-written tolerant scanner. Its agreement with the lexical
rules has no parser oracle and is instead pinned by a differential test
(`CompletionTokenizer - Prefixes Of Valid Expressions Lex Without Interior Errors`): every prefix of a
parser-accepted expression must tile contiguously with no interior error tokens, leaving only the final
in-progress tail unrecognized.
- **`app/runtime` (`CompletionService`, `QueryExpressionCompleter`, `MetadataValueCompleter`)** —
  composes the grammar catalog with the live library vocabulary (tags, custom keys, metadata
  values) and formats ranked `CompletionItem`s.
- **`app/linux-gtk` (`EntryCompletionController`)** — a framework adapter that drives a popover over
  a `Gtk::Entry` from a `Provider` callback. It owns the keyboard/pointer interaction only; it has
  no completion logic of its own. Completion rows render `displayText` as the primary label and
  `detail` as secondary text when provided.

## Quick Filter Control

The track quick filter is a compound input control: the internal `Gtk::Entry` owns typing,
drag-and-drop, and completion, while suffix icon buttons keep local actions visually attached to the
input. The clear button sits before the create-smart-list button because clearing edits the current
text, while creating a smart list derives a new list from the resolved expression. The create action
remains disabled until the view model reports a resolved expression that can be saved.

## Sigil triggers

| Sigil | Variable kind | Source |
|-------|---------------|--------|
| `$`   | Metadata      | `lib/query` catalog (canonical names + aliases) |
| `@`   | Property      | `lib/query` catalog (canonical names + aliases) |
| `#`   | Tag           | `CompletionService::tags()` (library vocabulary) |
| `%`   | Custom key    | `CompletionService::customKeys()` (library vocabulary) |

A trigger only completes when it opens a fresh token: the token tokenizer rejects positions inside a
quoted string and positions glued to a preceding identifier character, so `#"Rock"` and `foo$artist`
do not complete.

## Operator completion

After a complete lvalue variable, query completion offers the valid operator subset for that field.
An empty operator prefix is intentional: typing `$artist ` opens the operator menu even before an
operator character has been entered. Applying a binary operator replaces the whitespace and typed
operator prefix with a spaced operator token (for example `$artist i` -> `$artist in `); applying
`?` produces the postfix existence form (`$artist?`).

The catalog is field-aware:

- String, dictionary, and custom fields: `=`, `!=`, `~`, `in`, `?`
- Numeric and duration fields: `=`, `!=`, `<`, `<=`, `>`, `>=`, `in`, `?`
- Tag variables: `?`

Bare tag variables are complete predicates in the query language, so `#rock ` opens logical
operator completion directly. The postfix form `#rock?` remains accepted and is still treated as a
complete expression.

Keyword operators still obey query-language word boundaries. `$artistin` is treated as the variable
name `artistin`, not as `$artist in`.

After a complete expression, query completion offers the logical infix operators `and`, `or`, `&&`,
and `||`. An empty prefix is intentional here too: typing `$artist = "Miles" ` opens the logical
operator menu. Applying a logical operator replaces the whitespace and typed prefix with a spaced
operator token. Prefix unary operators (`not`, `!`) are not offered from this connection point.

## Query value completion

After a complete binary operator, query completion can offer values for dictionary-backed metadata
fields. The analyzer also recognizes values inside `in [...]` lists, including later comma-separated
items, and keeps them bound to the left-hand field.

Runtime value suggestions use the same whitelisted vocabulary as metadata-value completion:
Artist, Album, AlbumArtist, Genre, Composer, and Work
(`trackFieldSupportsValueCompletion`). Inserted query values are serialized string
constants, so a library value such as `Massive Attack` is inserted as `"Massive Attack"` rather than
as an invalid bare token. Custom values and numeric/unit templates are intentionally not populated
yet; the analyzer can identify those contexts, but `QueryExpressionCompleter` returns no items.

## Alias expansion (`$`/`@`)

Aliases match **exact-only**, canonical names match **by prefix**. Matching is the union of the two,
de-duplicated by canonical name, with exact-alias matches ranked ahead of canonical-prefix matches
(both in catalog order). The inserted and displayed text is **always the canonical name**: typing the
short alias and pressing <kbd>Tab</kbd> expands it, e.g. `$tn` ⇥ `$trackNumber`, `@br` ⇥ `@bitrate`.
This is a user-facing behavior added by the completion engine; the aliases themselves are accepted by
the parser independently.

## Metadata-value vocabulary

Value completion is deliberately limited in v1:

- Whitelisted fields only: Artist, Album, AlbumArtist, Genre, Composer, Work
  (`trackFieldSupportsValueCompletion`). High-cardinality fields (Title, Movement) and
  numeric fields are excluded.
- The vocabulary is built by a full library scan, frequency-ranked (descending, then value
  ascending), and cached until invalidated.
- Invalidation is coarse: any `LibraryMutationService::tracksMutated` marks every vocabulary dirty
  and the next access rebuilds. Rebuild counting uses hash maps, then applies the stable final sort.
  Dirty metadata-value vocabularies are rebuilt per storage tier: Artist, Album, AlbumArtist, Genre,
  and Composer share one hot-store scan; Work uses a cold-store scan. The `span<TrackId>` is retained
  for a future incremental path but unused today.
- A value editor holds a single value, so applying a suggestion replaces the **entire** entry text
  (the prefix matched is the text up to the cursor; any trailing text is discarded by design).

## Case folding is ASCII-only, on purpose

Prefix matching folds case with ASCII rules only (`café` and `CAFÉ` are treated as distinct). This is
**intentional consistency** with the query evaluator: `lib/query/ExecutionPlan.cpp` normalizes values
for comparison with the same ASCII folding. Making completion Unicode-aware in isolation would let it
suggest matches the actual filter would not make (or hide ones it would), desyncing suggestion from
result. A Unicode upgrade, if ever wanted, must be made in the evaluator and completion together.

## Threading

`CompletionService` keeps its caches **without synchronization** and asserts it is only ever touched
on its owning (main) thread. This holds because every `tracksMutated` emit is marshalled onto the
main thread before the subscription fires (synchronous mutators run on the UI thread; async scan
paths `resumeOnCallbackExecutor()` before emitting). The assert converts any future off-thread emit
into a loud failure rather than a silent data race.
