---
id: presentation.track-filter
type: spec
status: current
domain: presentation
summary: Defines quick-filter input resolution and completion, runtime view filtering, status, and Smart List derivation policy.
---
# Track filtering

## Scope

This specification defines the frontend-neutral behavior of the interactive track filter.
It owns classification of raw input as empty, quick search, or explicit expression; quick-term expansion and completion; runtime view replacement; filter status; and the policy for deriving a Smart List draft.

The complete expression syntax and predicate truth rules belong to the [predicate language](../../reference/query/predicate-language.md) and [predicate evaluation](../query/predicate-evaluation.md) owners.
Smart-list persistence and source membership belong to the [list model](../../reference/library/model/list.md) and [track source](../library/source/track-source.md) contracts.
Presentation grouping and ordering belong to [track-list presentation](track-presentation.md).

## Code boundary

This contract spans the **UIModel**, **application runtime**, and frontend layers from the [system architecture](../../architecture/system-overview.md), refined by the [track expression](../../architecture/track-expression.md), [library](../../architecture/library.md), and [presentation](../../architecture/presentation.md) architectures.
UIModel filter policy is public under `app/include/ao/uimodel/library/track/`; runtime view filtering is public through `app/include/ao/rt/ViewService.h`; GTK and TUI adapt those boundaries.

## Terminology

- **Raw filter** is the text retained by the interactive control.
- **Resolved expression** is the predicate text sent to `ViewService`.
- **Quick mode** expands plain search terms into predicates over common text fields and tags.
- **Expression mode** passes text with an explicit leading query variable through unchanged.
- **Aggregate vocabulary** is the distinct set of live values from the Quick-filter fields and tags, with occurrence frequencies merged across those sources.
- **Base source** is the source for the view's current `ListId` before the transient filter.

## Invariants

- Filtering changes membership but does not choose or persist a presentation.
- Clearing the filter restores the base source while retaining the active presentation.
- A transient filter is represented by base `ListId` plus expression text; it is not a stored Smart List.
- GTK and TUI use the same UIModel resolver.
- GTK and TUI use the same UIModel completer and therefore expose the same value set, ranking, replacement, and expression boundary.
- Runtime evaluates resolved text through the same source and predicate path used by Smart Lists.
- An invalid expression is observable in the view state and empty filtered membership without corrupting the base source.
- Creating a Smart List uses the resolved expression, not the user's unresolved quick-search text.

## Input resolution

Leading and trailing whitespace is removed first.
An empty result resolves to `None` with an empty expression.

Input resolves to `Expression` only when its first meaningful token is a query variable beginning with `$`, `@`, `#`, or `%`.
Whitespace, opening parentheses, and unary `not`/`!` may precede that variable.
Classification is a UI authoring boundary rather than expression validation: punctuation later in plain text does not switch modes, so values such as `P!nk`, `Live (1999)`, and `A+B` remain Quick filters.

Other input resolves to `Quick`.
Quick input splits on whitespace outside single or double quotes, removes the quote delimiters, decodes the query serializer's quoted-string escapes, and ignores empty terms.
Every term becomes a parenthesized disjunction over:

```text
$title
$artist
$album
$albumArtist
$genre
$composer
$work
#tag
```

The quick-search policy retains that list as typed runtime `TrackField` values and derives each canonical `$` variable through the field's typed query bridge.
The field list remains UIModel authoring policy rather than a query-language capability flag.

Text fields use `~`; the tag branch uses a serialized tag variable.
Multiple terms are parenthesized and joined with `and`.

The resolver serializes each term as an expression string constant and serializes the tag branch through the core query serializer.
The generated expression therefore preserves backslashes and both quote styles without substituting term content.
The tolerant splitter still accepts an unmatched quote through end of input; strict authoring errors remain proposal work.

## Completion

`TrackFilterCompleter` applies the same mode boundary as the resolver.
Expression-mode text delegates unchanged to `QueryExpressionCompleter`.
Quick-mode text completes the non-empty term at the cursor from live titles, artist, album, album artist, genre, composer, work, and tag values.
It does not include merely queryable fields such as conductor, ensemble, movement, soloist, or technical properties.

Matching is ASCII-case-insensitive prefix matching.
Identical text contributed by several fields, tags, or tracks is one candidate whose frequency is the sum of those live occurrences.
Candidates rank by descending frequency and then ascending value, and the requested result limit is applied after ranking.
An empty term does not open an arbitrary whole-library candidate list.

A selected value is serialized as one quoted string term and replaces only the term containing the cursor, including any unfinished quoted text or suffix after the cursor.
The resolver decodes that serialized term before building its predicate, so spaces, quotes, backslashes, and control escapes round-trip without changing the Quick-filter meaning.
The runtime owns aggregate vocabulary storage and invalidation; the selected field set, expression boundary, ranking use, and insertion policy remain UIModel behavior.

## State model

`TrackFilterViewModel` retains active `ViewId`, raw entry text, resolved expression, and optional filter error.
No focused view disables the control and clears its state.

Updating raw text resolves it immediately and calls synchronous `ViewService::setFilter()` with either an empty expression or the resolved expression.
On success the model reads the installed expression error from `TrackListViewState`; a command failure becomes the displayed error without replacing the preceding runtime resources.
The model renders once for the completed call.

The frontend-neutral view exposes:

- raw entry text;
- resolved expression;
- enabled state;
- error presence and tooltip;
- whether Create Smart List is allowed.

Create Smart List is allowed only when the resolved expression is non-empty and the current state has no error.

## Runtime transition

For a changed expression, `ViewService` acquires an ad-hoc source over the existing base list, constructs a new `LiveTrackListProjection` with the existing `TrackPresentationSpec`, and then installs the resources atomically into the view entry.

After installation it updates expression text and its optional expression error, then publishes the projection replacement.
An identical expression is a no-op.
A missing or source-less view returns a typed error without installing partial resources.

The ad-hoc source is weak-cached by `(baseListId, filterExpression)` while leased.
Its detailed membership and expression-error behavior is owned by [track sources](../library/source/track-source.md).

## Failure and cancellation

Quick-mode resolution itself does not parse or reject the produced text.
Parsing and compilation happen in the runtime source path.

An invalid expression remains an accepted view-filter state with an attached `FormatRejected` error and empty filtered membership.
Resource-acquisition failures that prevent construction return a failed `Result` and preserve the preceding installed view resources.

Resolution, source compilation, and view replacement are synchronous on their owning path and have no cancellation point.

## Persistence and versioning

Quick filters do not create independent presentation preferences or library list records.
Workspace/navigation state may retain base list, expression text, and exact presentation snapshot according to its owning persistence contract.

Choosing Create Smart List opens the ordinary list mutation flow using the resolved expression.
Only a successful library mutation turns that text into durable list data.

## Frontend observations

GTK binds the shared track-filter completer to its entry, renders clear and Create Smart List actions, and keeps Create disabled while the current expression is empty or erroneous.
The clear action submits empty text.
The create action emits the resolved expression.

TUI routes bare filter drafts and `/filter` arguments through the same completer and resolves submitted text through the same UIModel helper before calling `ViewService`.
Frontend-specific debounce, focus styling, popover rendering, and command syntax remain adapter concerns.

## Implementation map

- [`TrackFilterResolver`](../../../app/include/ao/uimodel/library/track/TrackFilterResolver.h) defines input modes and resolution.
- [`TrackFilterCompleter`](../../../app/include/ao/uimodel/library/track/TrackFilterCompleter.h) composes Quick and expression completion.
- [`TrackFilterViewModel`](../../../app/include/ao/uimodel/library/track/TrackFilterViewModel.h) owns frontend-neutral filter state.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) owns runtime view replacement and observations.
- [`TrackSourceCache`](../../../app/include/ao/rt/source/TrackSourceCache.h) owns ad-hoc source acquisition.
- [`TrackQuickFilter`](../../../app/linux-gtk/track/TrackQuickFilter.h) and [`CommandCompletionProvider`](../../../app/tui/CommandCompletionProvider.h) are completion adapters; `LibraryController` applies submitted TUI filters.

## Test map

- [`TrackFilterResolverTest.cpp`](../../../test/unit/uimodel/library/track/TrackFilterResolverTest.cpp) protects classification, exact mixed-quote preservation, serialized syntax, and expansion.
- [`TrackFilterCompleterTest.cpp`](../../../test/unit/uimodel/library/track/TrackFilterCompleterTest.cpp) protects the live field set, ranking, limits, replacement, escaping, and Quick/expression boundary.
- [`TrackFilterViewModelTest.cpp`](../../../test/unit/uimodel/library/track/TrackFilterViewModelTest.cpp) protects state, synchronous failure handling, and single-render policy.
- View-service and source tests under [`test/unit/runtime/`](../../../test/unit/runtime/) protect runtime replacement and membership.
- GTK quick-filter and TUI library-controller tests protect frontend adaptation.

## Related documents

- [Track expression architecture](../../architecture/track-expression.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Predicate language](../../reference/query/predicate-language.md)
- [Predicate evaluation](../query/predicate-evaluation.md)
- [Track sources](../library/source/track-source.md)
- [List presentation preference](list-preference.md)
