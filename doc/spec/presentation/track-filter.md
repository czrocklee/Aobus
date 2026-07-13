---
id: presentation.track-filter
type: spec
status: current
domain: presentation
summary: Defines quick-filter input resolution, runtime view filtering, status, and Smart List derivation policy.
---
# Track filtering

## Scope

This specification defines the frontend-neutral behavior of the interactive track filter.
It owns classification of raw input as empty, quick search, or explicit expression; quick-term expansion; runtime view replacement; filter status; and the policy for deriving a Smart List draft.

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
- **Expression mode** passes expression-like text through unchanged.
- **Base source** is the source for the view's current `ListId` before the transient filter.

## Invariants

- Filtering changes membership but does not choose or persist a presentation.
- Clearing the filter restores the base source while retaining the active presentation.
- A transient filter is represented by base `ListId` plus expression text; it is not a stored Smart List.
- GTK and TUI use the same UIModel resolver.
- Runtime evaluates resolved text through the same source and predicate path used by Smart Lists.
- An invalid expression is observable as filter status and empty filtered membership without corrupting the base source.
- Creating a Smart List uses the resolved expression, not the user's unresolved quick-search text.

## Input resolution

Leading and trailing whitespace is removed first.
An empty result resolves to `None` with an empty expression.

Input containing any expression marker resolves to `Expression` and is passed through after trimming.
The current marker set is `$`, `@`, `#`, `%`, `=`, `!`, `<`, `>`, `~`, `(`, `)`, `&`, `|`, and `+`.
Classification is a UI authoring heuristic; it does not validate the expression.

Other input resolves to `Quick`.
Quick input splits on whitespace outside single or double quotes, removes the quote delimiters, and ignores empty terms.
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

Text fields use `~`; the tag branch uses a serialized tag variable.
Multiple terms are parenthesized and joined with `and`.

The resolver quotes each term as an expression string and serializes the tag branch so spaces and ordinary quoted names remain valid query syntax.

## State model

`TrackFilterViewModel` retains active `ViewId`, raw entry text, resolved expression, pending state, and optional filter error.
No focused view disables the control and clears its state.

Updating raw text resolves it immediately, marks the local view state pending, and calls `ViewService::setFilter()` with either an empty expression or the resolved expression.
`FilterStatusChanged` for another view is ignored.
The matching status clears or retains pending according to runtime state and installs the optional expression error.

The frontend-neutral view exposes:

- raw entry text;
- resolved expression;
- enabled and pending state;
- error presence and tooltip;
- whether Create Smart List is allowed.

Create Smart List is allowed only when the resolved expression is non-empty and the current state is neither pending nor erroneous.

## Runtime transition

For a changed expression, `ViewService` acquires an ad-hoc source over the existing base list, constructs a new `LiveTrackListProjection` with the existing `TrackPresentationSpec`, and then installs the resources atomically into the view entry.

After installation it updates expression text, advances the view revision, and publishes filter, projection, and filter-status observations.
An identical expression is a no-op.
A missing, destroyed, or source-less view returns a typed error without installing partial resources.

The ad-hoc source is weak-cached by `(baseListId, filterExpression)` while leased.
Its detailed membership and expression-error behavior is owned by [track sources](../library/source/track-source.md).

## Failure and cancellation

Quick-mode resolution itself does not parse or reject the produced text.
Parsing and compilation happen in the runtime source path.

An invalid expression remains an accepted view-filter state with an attached `FormatRejected` error and empty filtered membership.
Resource-acquisition failures that prevent construction return a failed `Result` and preserve the preceding installed view resources.

Resolution, source compilation, view replacement, and status publication are synchronous on their owning path and have no cancellation point.

## Persistence and versioning

Quick filters do not create independent presentation preferences or library list records.
Workspace/navigation state may retain base list, expression text, and exact presentation snapshot according to its owning persistence contract.

Choosing Create Smart List opens the ordinary list mutation flow using the resolved expression.
Only a successful library mutation turns that text into durable list data.

## Frontend observations

GTK binds the shared query completer to its entry, renders clear and Create Smart List actions, and keeps Create disabled until UIModel allows it.
The clear action submits empty text.
The create action emits the resolved expression.

TUI resolves its filter draft through the same UIModel helper before calling `ViewService` and reports whether the result was a cleared, quick, or explicit filter.
Frontend-specific debounce, focus styling, popover rendering, and command syntax remain adapter concerns.

## Implementation map

- [`TrackFilterResolver`](../../../app/include/ao/uimodel/library/track/TrackFilterResolver.h) defines input modes and resolution.
- [`TrackFilterViewModel`](../../../app/include/ao/uimodel/library/track/TrackFilterViewModel.h) owns frontend-neutral filter state.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) owns runtime view replacement and observations.
- [`TrackSourceCache`](../../../app/include/ao/rt/source/TrackSourceCache.h) owns ad-hoc source acquisition.
- [`TrackQuickFilter`](../../../app/linux-gtk/track/TrackQuickFilter.h) and [`LibraryController`](../../../app/tui/LibraryController.h) are frontend adapters.

## Test map

- [`TrackFilterResolverTest.cpp`](../../../test/unit/uimodel/library/track/TrackFilterResolverTest.cpp) protects classification, quoting, and expansion.
- [`TrackFilterViewModelTest.cpp`](../../../test/unit/uimodel/library/track/TrackFilterViewModelTest.cpp) protects state and status policy.
- View-service and source tests under [`test/unit/runtime/`](../../../test/unit/runtime/) protect runtime replacement and membership.
- GTK quick-filter and TUI library-controller tests protect frontend adaptation.

## Related documents

- [Track expression architecture](../../architecture/track-expression.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Predicate language](../../reference/query/predicate-language.md)
- [Predicate evaluation](../query/predicate-evaluation.md)
- [Track sources](../library/source/track-source.md)
- [List presentation preference](list-preference.md)
