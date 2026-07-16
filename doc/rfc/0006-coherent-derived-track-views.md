---
id: rfc.0006.coherent-derived-track-views
type: rfc
status: draft
domain: query
summary: Proposes revisioned asynchronous filter and Smart List materialization with coherent status and runtime-owned preview.
depends-on: rfc.0009.pure-expression-binding
---
# RFC 0006: Coherent derived track views

## Problem

Interactive filters and Smart Lists share predicate-backed `TrackSource` machinery, but their application boundary is neither coherent nor responsive.

`TrackFilterViewModel::updateFilter()` marks its local state pending, ignores the `ViewService::setFilter()` result, and relies on a later status event to clear pending.
`ViewService::setFilter()` returns immediately without an event when the resolved expression is unchanged.
Submitting identical text, changing only whitespace that resolves to the same expression, or receiving an early command failure can therefore leave Create Smart List disabled by a permanently pending UIModel state.

Runtime `FilterStatusChanged::pending` does not model real work.
The current filter command synchronously acquires an ad-hoc source, compiles its expression, reads the complete upstream source, evaluates every required track, constructs a projection, and only then returns.
Large libraries can block the interactive callback thread during quick filtering, Smart List opening, and preview rebuild.

Quick-filter authoring still maintains a tolerant quote splitter separate from the core query lexer.
Generated system variables now use the core variable formatter, while tag variables and string constants use the core serializer, so terms containing both quote styles are preserved exactly, but unmatched quotes remain accepted without an explicit authoring result.

Finally, GTK `SmartListDialog` constructs `SmartListEvaluator` and `SmartListSource` directly against `AppRuntime::musicLibrary()` for preview.
That frontend seam duplicates runtime source composition and prevents one cancellation, revision, and stale-result policy from covering ordinary views and previews.

## Dependencies

- Hard: [RFC 0009](0009-pure-expression-binding.md) supplies plan-owned symbols and explicit dictionary contexts required to complete worker-safe materialization.
- Conditional: None.
- Integration: [RFC 0008](0008-declarative-track-capability-bridge.md) supplies typed field identities for the UIModel-owned quick-search field set without changing this RFC's command or execution protocol.

## Goals

- Make filter command acceptance, pending state, completion, and failure one revisioned runtime transaction.
- Preserve user search terms exactly while producing syntactically valid query expressions.
- Move initial and replacement derived-membership rebuilds off the interactive callback executor.
- Keep the previous committed projection visible until a replacement is ready or the user clears it.
- Reject stale worker results after newer filter, source, library, view, or shutdown revisions.
- Reuse one runtime-owned derived-view service for stored Smart Lists, ad-hoc filters, and Smart List preview.
- Preserve the independent presentation axis while membership is pending or replaced.
- Retain current incremental source updates after initial materialization.

## Non-goals

- Changing predicate grammar, truth semantics, or the Smart List inheritance model.
- Moving presentation sorting, grouping, or field selection into the expression system.
- Replacing the general async runtime or `LibraryTaskService` proposal from RFC 0004.
- Designing incremental completion vocabulary; that belongs to [RFC 0007](0007-revisioned-completion-vocabulary.md).
- Finalizing pure query symbol binding; [RFC 0009](0009-pure-expression-binding.md) owns that prerequisite for worker-safe plans.
- Persisting speculative filter requests or worker-internal execution plans.

## Proposed design

### Runtime command and state owner

`ViewService` remains the owner of open view identity and committed projection state.
It delegates predicate materialization to a new runtime-owned `DerivedTrackViewService` composed by `CoreRuntime` beside `TrackSourceCache`.

Every filter request receives a monotonically increasing `FilterRequestId` scoped to its `ViewId`.
Runtime publishes one self-contained state:

```cpp
enum class FilterPhase
{
  Idle,
  Pending,
  Ready,
  Rejected,
};

struct TrackFilterSnapshot final
{
  ViewId viewId;
  FilterRequestId requestId;
  std::uint64_t viewRevision;
  std::string requestedExpression;
  std::string committedExpression;
  FilterPhase phase;
  std::optional<Error> error;
};
```

The exact type names may change during implementation, but requested and committed expression must remain distinct.
UIModel derives pending, error, tooltip, and Create Smart List availability from this runtime snapshot instead of maintaining an optimistic parallel state machine.

`setFilter()` returns an explicit command outcome for every request:

- `Accepted(requestId)` when new work starts;
- `AlreadyCurrent(snapshot)` when the resolved expression is already committed;
- `AlreadyPending(requestId)` when it matches the active request;
- typed failure when the view cannot accept a request.

No caller relies on the presence of a later event merely to recover from a no-op or rejected command.

### Authoring without text mutation

UIModel retains the choice between explicit-expression and quick-search modes, but quick term tokenization becomes a dedicated authoring component with an explicit result.

The component preserves the exact Unicode term text and constructs the expanded AST directly:

```text
raw input
  -> quick-term tokenizer
  -> vector<string> terms or authoring error
  -> query AST nodes
  -> canonical query serializer
```

It does not build expression strings with manual quote selection.
Quoted term delimiters are removed only when a matching closing delimiter exists.
Backslash and quote behavior is defined once by this authoring contract and covered with differential tests against `query::parse(query::serialize(ast))`.

The current resolver already delegates system-variable formatting and tag-variable and string-constant encoding to the core query layer and has a mixed-quote regression.
This proposal retains only the dedicated tokenizer, explicit authoring result, direct AST composition, and broader differential coverage.

Expression-mode detection remains UIModel policy, not parser grammar.
[RFC 0008](0008-declarative-track-capability-bridge.md) replaced the hard-coded source-variable strings with an explicit typed runtime-field list without changing this command protocol.

### Asynchronous materialization

The callback executor performs only bounded capture and commit work:

1. Validate that the view and base source are live.
2. Allocate `FilterRequestId`, install `Pending`, and publish the snapshot.
3. Capture an immutable source descriptor containing base source identity, source revision, ordered track ids, resolved expression, library revision, and cancellation generation.
4. Compile/bind the predicate into a worker-safe plan.
5. Evaluate the captured ordered ids using a worker-owned read transaction and the plan's access profile.
6. Return an immutable ordered membership result to the callback executor.
7. Revalidate request, view, source, library, and shutdown revisions.
8. Construct or adopt the derived source, combine it with the existing `TrackPresentationSpec`, and atomically install the replacement projection.
9. Advance the view revision and publish one `Ready` snapshot plus the replacement projection.

The worker never calls a mutable `TrackSource`, `ViewService`, projection, UIModel, or frontend object.
It receives value snapshots and an explicit library-lifetime token owned by the runtime task graph.

[RFC 0009](0009-pure-expression-binding.md) removes mutable dictionary interning and raw dictionary pointers from compiled plans.
Until that prerequisite is implemented, the asynchronous phase cannot move binding or evaluation to a worker merely by passing the current plan type across threads.

### Supersession and source changes

A newer filter request cancels or invalidates the older request id.
Clear-filter is a new request that can synchronously reinstall the already leased base source and cancel pending worker work.

If the base source or library revision changes while a rebuild is in flight, the result is stale.
Runtime either schedules a fresh capture for the still-current request or leaves it pending until the source is live, according to a bounded retry policy.
It never publishes membership computed from one source revision as if it belonged to another.

After a derived source becomes live, the existing `SmartListEvaluator` incremental delta behavior remains the authority for ordinary upstream batches.
A reset or expression replacement can use the asynchronous rebuild path again.

### Rejection behavior

Syntax or compilation failure publishes `Rejected` for the requested expression without replacing the committed projection.
The user sees the error while the last valid result remains visible.

This deliberately changes the current invalid-expression behavior, which installs an empty derived source.
It prevents a transient editing error from making a populated view appear authoritatively empty.
Create Smart List remains disabled while the latest request is pending or rejected.

Runtime resource or read failure follows the same requested-versus-committed rule and preserves the previous committed projection.
Clear-filter remains available from every phase.

### Runtime-owned Smart List preview

`DerivedTrackViewService` exposes a preview lease that accepts a base source descriptor plus draft expression and returns the same revisioned phases and ordered result with an optional result limit.

GTK `SmartListDialog` consumes that lease through runtime or a narrow UIModel preview model.
It no longer constructs `SmartListEvaluator`, holds `MusicLibrary&`, or decides worker lifetime.
Closing the dialog cancels its lease; stale completion cannot update a destroyed dialog.

The preview result is non-durable and never publishes a library mutation.
Submitting the draft still uses `LibraryWriter`, which validates the expression again at the commit boundary.

### Execution and shutdown

Derived-view jobs use the shared async runtime with cooperative stop tokens and callback-executor completion.
`CoreRuntime` closes the derived-view admission gate before stopping workers, waits for captured library ownership to quiesce, and destroys sources only after callbacks can no longer arrive.

Worker concurrency is bounded.
Rapid filter typing coalesces superseded work per view instead of filling the worker queue with every debounce result.

## Alternatives

### Fix only the pending no-op

Returning or emitting a snapshot for identical expressions fixes one bug but retains synchronous full-library work, duplicated preview composition, and ambiguous requested-versus-committed state.
It is an acceptable first implementation phase, not the final design.

### Keep synchronous evaluation and remove pending

This accurately describes the present implementation but accepts UI stalls as product behavior and leaves no path for cancellation or stale-result suppression.

### Show an empty projection while pending or invalid

Empty output cannot distinguish “no tracks match” from “the new predicate is not ready” or “the expression is invalid.”
Keeping the last committed projection plus explicit phase is more coherent.

### Put preview evaluation in UIModel

UIModel would still need low-level library/source access and worker lifetime, violating the established layer boundary.
Runtime owns the reusable mechanism; UIModel owns draft and display policy.

### Create a second asynchronous source type

Parallel synchronous and asynchronous smart-source implementations would duplicate incremental and invalidation contracts.
The selected design adds asynchronous materialization behind the existing source boundary.

## Compatibility and migration

No library record or workspace schema change is required.
Persisted Smart List and committed filter expression text retain their current meaning.

Implementation proceeds in phases:

1. Make filter command results explicit, eliminate UIModel-owned speculative pending, and add identical-expression and error regression tests.
2. Replace manual quick-term string construction with AST plus canonical serialization.
3. Introduce `FilterRequestId`, requested/committed snapshots, and last-valid-result rejection behavior while evaluation remains synchronous.
4. Add the runtime preview lease and migrate GTK off direct `MusicLibrary`/evaluator construction.
5. After RFC 0009 provides worker-safe binding, move initial and replacement rebuilds to bounded worker execution with generation barriers.
6. Route Smart List resets and expensive rebuilds through the same materialization boundary where scale evidence justifies it.

During migration, no new frontend may construct a concrete smart evaluator or use `CoreRuntime::musicLibrary()` for preview.

## Validation

- Repeating identical raw and resolved filters always produces a non-pending coherent snapshot.
- A synchronous command failure cannot strand UIModel pending state.
- Terms containing either or both quote styles, backslashes, whitespace, and Unicode round-trip through AST serialization without text substitution.
- Unterminated quick-search quotes produce an explicit authoring result rather than silently changing term boundaries.
- Invalid expressions preserve the preceding committed projection and publish `Rejected` with the requested text.
- Clear-filter cancels in-flight work and reinstalls the base source deterministically.
- A newer request, view destruction, list deletion, source invalidation, library reset, or shutdown prevents stale installation.
- Presentation id, grouping, sorting, and visible fields remain unchanged across pending and successful filter replacement.
- A controlled slow track read does not block callback-executor heartbeat, playback commands, or unrelated views.
- Rapid typing produces bounded active/queued work and only the final current request may install.
- Smart List preview uses no direct GTK storage/source dependency and cannot callback after dialog destruction.
- Existing source delta, Smart List inheritance, quick-filter, presentation, and workspace tests remain behavior oracles where this RFC does not intentionally change rejection display.
- New concurrency contracts pass the repository ThreadSanitizer workflow, and completed implementation passes `./ao check`.

## Open questions

- Should source revision change automatically retry the current request once, or wait for a later stable source notification?
- What result limit should preview apply before or after predicate evaluation, and must it preserve a total match count?
- Should explicit-expression detection remain punctuation-based, or should authoring attempt full parse before choosing quick mode?
- Does a stored Smart List opening with an invalid expression retain an empty source, while only transient editing preserves last valid content, or should both use the same rejected-state model?

## Promotion plan

If accepted and implemented, update:

- [Track expression architecture](../architecture/track-expression.md) with derived-view execution and ownership;
- [Library architecture](../architecture/library.md) with the materialization service and lifetime;
- [Presentation architecture](../architecture/presentation.md) after the GTK preview seam is removed;
- [Track filtering](../spec/presentation/track-filter.md) with request and phase behavior;
- [Track sources](../spec/library/source/track-source.md) with asynchronous materialization and stale-result rules;
- [Track-list projection](../spec/library/projection/track-list.md) with replacement commit behavior;
- development concurrency guidance if the new worker/callback pattern adds a reusable contract.
