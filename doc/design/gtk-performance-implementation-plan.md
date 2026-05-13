# GTK Frontend Performance Implementation Plan

## Purpose

This document is the revised implementation plan for improving the GTK track list performance.
It replaces the earlier draft assumptions with the constraints verified in the current codebase.

The overall direction remains valid:

- avoid per-keystroke UI-thread filtering in `TrackListAdapter`;
- use runtime projections as the source of ordering/index information;
- reduce `Gio::ListStore` signal storms during projection delta handling;
- stop front-loading all `TrackRow` creation at application startup.

However, the implementation must not assume that routing filter text through `ViewService` is
automatically O(1) or non-blocking. Today, `ViewService::setFilter()` calls
`SmartListSource::reload()`, and `SmartListEvaluator::rebuildGroup()` synchronously scans the
source and reads LMDB on the calling thread.

---

## 1. Verified Problems

| ID | Problem | Severity | Verified cause | User impact |
|---|---|---:|---|---|
| P1 | Filter entry can freeze the UI | Critical | `TrackViewPage::onFilterChanged()` calls `TrackListAdapter::setFilter()`, which schedules a rebuild that scans the source and evaluates the filter on the GTK thread. Moving to `ViewService::setFilter()` is still synchronous today. | Large libraries can stall on each filter update. |
| P2 | Playing-row update scans the whole model | Critical | `TrackViewPage::setPlayingTrackId()` iterates every row. | Track changes can stutter with large lists. |
| P3 | Programmatic selection scans the whole model | Critical | `TrackViewPage::selectTrack()` iterates every row to find the target track. | Navigation and remote-control jumps can stall. |
| P4 | Startup preloads all `TrackRow` objects | Major | `TrackRowDataProvider::loadAll()` creates one GObject per track and is called during session initialization/import reloads. | High startup memory and startup latency. |
| P5 | Projection deltas are applied row-by-row | Major | `TrackListAdapter::bindProjection()` handles reset/insert/update/remove with repeated `append`, `insert`, `remove`, or one-row `splice`. | GTK emits many `items-changed` notifications and relayouts. |
| P6 | Filtering behavior is duplicated | Major | `TrackListAdapter` has a local filter pipeline while runtime also has `SmartListSource` and `SmartListEvaluator`. | Divergent behavior and duplicated maintenance. |
| P7 | Projection-bound adapter still observes the dummy source | Minor | `TrackPageGraph` constructs adapters with `allTracks()` as a dummy source, then calls `bindProjection()`. The adapter remains attached to the dummy source. | Confusing update paths and possible redundant rebuilds. |

---

## 2. Constraints and Corrections to the Previous Draft

### 2.1 `ViewService::setFilter()` is not currently non-blocking

Do not claim that moving filter routing from `TrackListAdapter` to `ViewService` makes filtering
O(1). It only moves ownership of filtering to the runtime layer.

Current synchronous path:

```text
TrackViewPage::onFilterChanged
  └─ ViewService::setFilter
       ├─ SmartListSource::setExpression
       └─ SmartListSource::reload
            └─ SmartListEvaluator::rebuildGroup
                 └─ for each source track: LMDB read + expression evaluation
```

### 2.2 `ViewService::setFilter()` can replace the projection

When a view changes from no filter to a filter, or from a filter back to no filter,
`ViewService::setFilter()` creates a new `TrackListProjection`. Existing GTK pages currently bind
their `TrackListAdapter` to the projection only once when the page is created.

Before GTK can depend on runtime filtering, one of these must be implemented:

1. **Preferred for minimal change:** add a post-update projection replacement notification from
   `ViewService`, and have `TrackPageGraph` rebind the page adapter to the new projection.
2. **Preferred for long-term architecture:** make `TrackListProjection` source-switchable while
   preserving subscriptions, then `ViewService` can update the source without replacing the
   projection object.

Do not only call `ViewService::setFilter()` from `TrackViewPage`; that leaves the page subscribed to
the old projection in the no-filter/filter boundary cases.

### 2.3 Smart-list error handling already exists, but no UI status signal exists

`SmartListSource` already exposes `hasError()` and `errorMessage()`. Parser/compiler exceptions are
caught inside `SmartListSource::stageExpression()`, so wrapping `setExpression()`/`reload()` in a
`try` block at `ViewService::setFilter()` will not reliably report expression errors.

Add an explicit runtime status path instead, for example:

```cpp
struct FilterStatusChanged final
{
  ViewId viewId{};
  std::string expression{};
  bool pending = false;
  bool hasError = false;
  std::string errorMessage{};
  std::uint64_t revision = 0;
};
```

The signal must be emitted after the relevant `SmartListSource` has applied the staged expression
and after any projection replacement has completed.

### 2.4 Runtime filtering may change row order unless fixed

The current local adapter filter scans `_source.trackIdAt(index)` and therefore preserves source
order for unsorted views. `SmartListSource` stores members in `std::flat_set<TrackId>`, so it exposes
matching tracks in `TrackId` order.

Before replacing local GTK filtering with runtime filtering, decide and test the ordering contract:

- If filtered manual lists must preserve source order, change `SmartListSource` to maintain an
  ordered member vector plus an index map, or otherwise make evaluator output source-order members.
- If projection sorting always defines user-visible order for the affected views, document that
  contract and add tests that prove the UI order remains acceptable.

### 2.5 Provider-level LRU alone cannot solve model memory

`TrackRowDataProvider` can evict rows from its cache, but the current `Gio::ListStore<TrackRow>` also
holds strong references to every row that was inserted into the model. A provider LRU does not free
rows still owned by the list model.

Therefore Phase 4 must be a model architecture change, not just a cache change.

---

## 3. Revised Implementation Phases

```text
Phase 0: Measurement and invariants
   │
   ├─▶ Phase 1: Runtime filter ownership and projection rebinding correctness
   │       │
   │       ├─▶ Phase 2: Responsive filter input and status reporting
   │       │
   │       └─▶ Phase 3: O(1) row lookup for selection and playing state
   │
   ├─▶ Phase 4: Batch projection delta application
   │
   └─▶ Phase 5: Lazy row/model architecture for memory reduction

Optional but likely for large libraries:
Phase 6: Background smart-list evaluation with cancellation
```

Phase 3 and Phase 4 can be implemented before full asynchronous filtering, as long as projection
rebinding correctness is handled first.

---

## 4. Phase 0: Measurement and Invariants

### Goal

Add enough measurements and tests to prevent performance work from being guided by assumptions.

### Tasks

1. Add lightweight timing logs around:
   - `TrackViewPage::onFilterChanged()`;
   - `ViewService::setFilter()`;
   - `SmartListEvaluator::rebuildGroup()`;
   - `TrackListProjection::rebuildOrderIndex()`;
   - `TrackListAdapter` projection delta application.
2. Add an internal invariant check in debug builds:

   ```text
   list model item count == bound projection size
   ```

   This is important because Phase 2 uses projection indices to address GTK model positions.
3. Add or extend scale tests around `TrackListProjection::indexOf()` and reset/update ranges.
4. Capture baseline numbers for at least 10k and 100k tracks:
   - startup time after session initialization;
   - peak RSS after first track page appears;
   - filter latency for a quick filter and an expression filter;
   - playing-track update latency;
   - projection reset application latency.

### Done criteria

- Baseline measurements are recorded in the issue/thread implementing the performance work.
- Tests cover the model/projection count invariant at the adapter level where practical.

---

## 5. Phase 1: Runtime Filter Ownership and Projection Rebinding Correctness

### Goal

Make runtime filtering the single source of truth without breaking existing GTK pages when the
underlying projection object changes.

### Tasks

#### 1.1 Move quick-filter resolution out of adapter state

The quick-filter expression builder currently lives in `TrackListAdapter.cpp` as local helper logic.
Expose it through one stable UI-facing API before removing adapter filter state.

Acceptable minimal options:

- make it a public static `TrackListAdapter::resolveFilterExpression(std::string_view)` while the
  adapter still owns adjacent logic; or
- move it into a small UI helper if that avoids keeping unrelated adapter API.

The returned value must include:

```cpp
enum class TrackFilterMode { None, Quick, Expression };

struct ResolvedTrackFilter final
{
  TrackFilterMode mode = TrackFilterMode::None;
  std::string expression{};
};
```

#### 1.2 Add projection replacement handling

Implement one of the two approaches below.

##### Minimal approach: projection replacement signal

Add a `ViewService` signal emitted after `setFilter()` finishes replacing or rebuilding the active
projection:

```cpp
struct TrackListProjectionChanged final
{
  ViewId viewId{};
  std::shared_ptr<ITrackListProjection> projection{};
  std::uint64_t revision = 0;
};
```

`TrackPageGraph` subscribes and, for the matching page:

1. calls `TrackListAdapter::bindProjection(newProjection)`;
2. forces the adapter to rebuild/reset its model from the new projection;
3. reapplies grouping header state and playing-row state;
4. keeps selection state consistent with `ViewService` selection state where possible.

`TrackListAdapter::bindProjection()` must reset any previous projection subscription before binding
the new one.

##### Long-term approach: switchable projection source

Refactor `TrackListProjection` to hold a `TrackSource*`, detach from the old source and attach to the
new source, rebuild order/index data, and publish a reset delta without replacing the projection
object. This avoids GTK rebinding complexity but touches more runtime code.

#### 1.3 Stop observing the dummy source when a projection is bound

When `TrackListAdapter::bindProjection()` succeeds, detach the adapter from the constructor-provided
source. The destructor should detach from the source only if the adapter is still source-bound.

Also ensure rebinding is safe:

```cpp
void TrackListAdapter::bindProjection(ao::rt::ITrackListProjection& projection)
{
  _rebuildConnection.disconnect();
  _projectionSub.reset();

  if (!_sourceDetachedForProjection)
  {
    _source.detach(this);
    _sourceDetachedForProjection = true;
  }

  _projection = &projection;
  _projectionSub = projection.subscribe(/* apply batch */);
  resetFromProjection();
}
```

#### 1.4 Preserve or explicitly define filtered ordering

Before routing `TrackViewPage` filters to `ViewService`, add tests that cover filtering a manual
source with non-TrackId order.

If the desired behavior is to preserve source order, change `SmartListSource` from only a
`std::flat_set<TrackId>` to an ordered membership representation:

```cpp
std::vector<TrackId> _membersInSourceOrder;
std::unordered_map<TrackId, std::size_t> _positionIndex;
```

This also makes `SmartListSource::indexOf()` O(1), which helps incremental update paths.

#### 1.5 Remove adapter-local filtering only after runtime path is correct

After projection rebinding and ordering are fixed:

- remove `TrackListAdapter::setFilter()` and its filter state;
- remove local filter branches from source-observer callbacks;
- keep source-observer behavior only for non-projection adapters, such as preview or temporary views
  that intentionally do not use runtime projections.

### Tests

- `ViewServiceTest`: no-filter → filter → filter update → clear filter keeps the GTK-relevant
  projection path observable through either a changed-projection signal or a stable projection reset.
- `SmartListSource`/runtime test: filtered manual list preserves the chosen ordering contract.
- GTK adapter test: rebinding projection disconnects old subscription and resets the model exactly
  once.

### Done criteria

- A `TrackViewPage` can route filter text to `ViewService` without becoming stuck on an old
  projection.
- Filtered row order is explicitly tested.
- Adapter-local filter evaluation is removed or isolated to non-runtime preview use cases.

---

## 6. Phase 2: Responsive Filter Input and Runtime Status

### Goal

Make filter typing responsive and make expression errors visible without relying on
`TrackListAdapter` state.

### Tasks

#### 2.1 Move filter UI state to `TrackViewPage`

`TrackViewPage` should keep:

```cpp
TrackFilterMode _filterMode = TrackFilterMode::None;
std::string _currentFilterExpression;
bool _filterPending = false;
bool _filterHasError = false;
std::string _filterErrorMessage;
std::uint64_t _filterRevision = 0;
```

`updateFilterUi()` should use this page/runtime state, not adapter APIs.

The “create smart list” icon is enabled only when:

- `_currentFilterExpression` is not empty;
- `_filterPending == false`;
- `_filterHasError == false`.

#### 2.2 Add debounce before calling runtime filtering

Connect the filter entry to a short debounce, for example 150-250 ms. Each new keystroke cancels the
previous pending apply.

This reduces per-keystroke work even before a background evaluator exists. It does not by itself
guarantee no UI freeze while `ViewService::setFilter()` is still synchronous.

#### 2.3 Add filter status signal

Add `ViewService::onFilterStatusChanged()` or equivalent. Emit status after expression staging and
reload/rebuild has completed, including parser/compiler errors already stored by `SmartListSource`.

If background evaluation is added later, include a revision token and ignore stale completions in the
GTK page.

#### 2.4 Decide whether background evaluation is part of this milestone

If acceptance requires “typing never blocks the GTK frame loop”, implement Phase 6 together with this
phase. If acceptance only requires removing duplicate adapter filtering and improving typical
responsiveness, debounce plus measurement may be enough for the first pass.

### Tests

- Unit test quick-filter resolution for plain text, quoted text, expression text, and empty text.
- ViewService status test for valid and invalid expressions.
- GTK-level test or focused integration test that `TrackViewPage` updates error CSS/status based on
  runtime status rather than adapter state.

### Done criteria

- Invalid expression status is visible in the filter entry/status banner.
- “Create smart list” uses the resolved runtime expression and is disabled for pending/error states.
- Measurements explicitly state whether filtering is still synchronous.

---

## 7. Phase 3: O(1) Selection and Playing-State Lookups

### Goal

Replace whole-model scans used for direct row targeting with projection-index lookups.

### Tasks

#### 3.1 Expose projection index lookup on the adapter

Add:

```cpp
std::optional<std::size_t> TrackListAdapter::indexOf(TrackId trackId) const
{
  if (_projection != nullptr)
  {
    return _projection->indexOf(trackId);
  }

  return _source.indexOf(trackId);
}
```

The fallback preserves behavior for non-projection adapters.

#### 3.2 Rewrite `TrackViewPage::selectTrack()`

Use `TrackListAdapter::indexOf()` and scroll/select the returned model position.

Guard the model/projection invariant before selecting:

```cpp
if (auto idx = _adapter.indexOf(trackId); idx && *idx < _selectionModel->get_n_items())
{
  _selectionModel->select_item(static_cast<guint>(*idx), true);
  _columnView.scroll_to(static_cast<guint>(*idx), nullptr,
                        Gtk::ListScrollFlags::FOCUS | Gtk::ListScrollFlags::SELECT, nullptr);
}
```

#### 3.3 Rewrite playing-row updates without caching stale indices

Do not store only `_playingRowIndex`; the index can become stale after filter/sort/group/projection
reset.

Store the current playing track ID:

```cpp
std::optional<TrackId> _playingTrackId;
```

When the playing track changes:

1. use `_adapter.indexOf(*_playingTrackId)` to clear the old row if it is still visible;
2. store the new track ID;
3. use `_adapter.indexOf(newTrackId)` to set the new row if it is visible.

After projection reset/rebind, call a small helper to reapply the current playing state to the new
projection. If needed, clear any visible rows that still have `playing=true` only during a full reset,
not on every track change.

#### 3.4 Optimize primary selected track and selected count with GTK bitsets

Use `Gtk::SelectionModel::get_selection()`/`Gtk::Bitset` APIs where available:

- `getPrimarySelectedTrackId()` can use the first selected bit instead of scanning every item.
- `selectedTrackCount()` can use the bitset size/count if gtkmm exposes it.

Keep a fallback scan for API compatibility if the binding does not expose a required bitset method.

#### 3.5 Revisit selected rows/IDs separately

`getSelectedTrackIds()`, `getSelectedRows()`, and `getSelectedTracksDuration()` may still need to
iterate selected positions. Improve them with bitset iteration if gtkmm exposes the API; otherwise
leave the scan but document that it is only used on explicit user actions such as context menus.

`getVisibleTrackIds()` is inherently O(N) because playback queue construction needs the full visible
order. Prefer reading directly from the projection:

```cpp
for (std::size_t i = 0; i < projection->size(); ++i)
{
  result.push_back(projection->trackIdAt(i));
}
```

This avoids materializing GTK row objects while preserving the same O(N) output size.

### Tests

- Unit or GTK test: `selectTrack()` selects the expected row after filtering/grouping.
- Test: changing playing track only toggles old and new rows when both are visible.
- Test: projection reset followed by playing-state reapply marks the correct row.

### Done criteria

- `setPlayingTrackId()` and `selectTrack()` no longer scan all model items.
- No stale-index bug after filter, grouping, sorting, or projection replacement.

---

## 8. Phase 4: Batch Projection Delta Application

### Goal

Reduce GTK model signal storms by applying projection deltas with the fewest safe `splice()` calls.

### Tasks

#### 4.1 Centralize delta handling

Move the `std::visit` logic in `TrackListAdapter::bindProjection()` into named private methods:

```cpp
void applyDeltaBatch(ao::rt::TrackListProjectionDeltaBatch const& batch);
void applyResetDelta();
void applyInsertRange(ao::rt::ProjectionInsertRange const& delta);
void applyRemoveRange(ao::rt::ProjectionRemoveRange const& delta);
void applyUpdateRange(ao::rt::ProjectionUpdateRange const& delta);
```

This makes rebinding, testing, and invariant checks easier.

#### 4.2 Use batch `splice()` for contiguous ranges

For reset:

```cpp
auto rows = buildRowsForRange(0, _projection->size());
_listModel->splice(0, _listModel->get_n_items(), rows);
```

For insert:

```cpp
auto rows = buildRowsForRange(delta.range.start, delta.range.count);
_listModel->splice(static_cast<guint>(delta.range.start), 0, rows);
```

For remove:

```cpp
_listModel->splice(static_cast<guint>(delta.range.start),
                   static_cast<guint>(delta.range.count), {});
```

For update:

```cpp
auto rows = buildRowsForRange(delta.range.start, delta.range.count);
_listModel->splice(static_cast<guint>(delta.range.start),
                   static_cast<guint>(delta.range.count), rows);
```

#### 4.3 Reuse a transaction while building rows

Add an overload to `TrackRowDataProvider::getTrackRow()` that accepts a caller-provided reader:

```cpp
Glib::RefPtr<TrackRow> getTrackRow(
  TrackId id,
  ao::library::TrackStore::Reader const* externalReader = nullptr) const;
```

The adapter should create one read transaction per contiguous range, not one transaction per row.

#### 4.4 Preserve model/projection size invariants

Do not silently insert fewer rows than the projection range count. If a row cannot be built for a
projection track, treat it as a consistency problem:

- log it with enough context;
- prefer a full reset/rebuild or placeholder row over creating an index mismatch;
- add a debug assertion that model count matches projection size after each batch.

### Tests

- Adapter test: reset emits one model change and produces the expected row count.
- Adapter test: insert/update/remove contiguous ranges preserve item order and count.
- Regression test: provider returning a missing row does not leave model/projection indices shifted.

### Done criteria

- Reset, insert, update, and remove ranges are handled with batch operations.
- Adapter index-based operations remain correct after every delta type.

---

## 9. Phase 5: Lazy Row/Model Architecture for Memory Reduction

### Goal

Actually reduce memory by avoiding one heavyweight `TrackRow` GObject per track in the GTK model.

### Important correction

Removing `TrackRowDataProvider::loadAll()` and adding LRU eviction to the provider cache is not
enough. The current `Gio::ListStore<TrackRow>` holds every inserted row, so those rows remain alive
even if the provider cache evicts them.

### Recommended design options

#### Option A: projection-backed lazy `Gio::ListModel`

Create a custom model that implements `GListModel`/gtkmm equivalent and is backed directly by the
bound projection:

- `get_n_items()` returns `projection->size()`;
- `get_item(position)` resolves `projection->trackIdAt(position)` and returns a cached/lazy
  `TrackRow`;
- projection deltas emit model `items-changed` without storing all rows in a `Gio::ListStore`;
- provider LRU can now release non-visible/non-recent rows because the model does not hold them all.

This is the best long-term fix and aligns with GTK's virtualized view model expectations.

#### Option B: lightweight model item plus lazy metadata

If a custom list model is too invasive, replace `TrackRow` in the list store with a much smaller
object that stores only `TrackId` and lazy property accessors. This still creates one GObject per
track, but can reduce memory substantially if full metadata strings and formatted fields are loaded
only for visible rows.

This is an intermediate step, not the final solution.

#### Option C: provider LRU only

Do not treat this as a memory fix while the list store owns all rows. Provider LRU is useful only
after Option A or a similar lazy model prevents the model from holding every row.

### Tasks

1. Prototype Option A behind `TrackListAdapter` so `TrackViewPage` does not need large changes.
2. Move row creation into lazy `get_item()`/visible-row binding paths.
3. Keep `TrackRowDataProvider::invalidate()` and `remove()` semantics for mutation handling.
4. Add a bounded LRU only after the model no longer owns all rows.
5. Remove startup `loadAll()` calls once the lazy model can populate visible rows on demand.

### Tests

- Model test: item count tracks projection size without materializing all rows.
- Provider test: LRU eviction removes cached rows while visible rows remain alive through GTK refs.
- Integration/scale test: opening a 100k-track page creates far fewer than 100k `TrackRow` objects.
- Mutation test: invalidating a visible row refreshes the displayed metadata.

### Done criteria

- Startup no longer calls `loadAll()`.
- Opening a large track page does not create one `TrackRow` per track.
- Memory measurements show a real reduction compared with Phase 0 baseline.

---

## 10. Phase 6: Background Smart-List Evaluation with Cancellation

### Goal

Make filter evaluation non-blocking for large libraries.

### When to implement

Implement this phase in the same milestone as Phase 2 if the acceptance target is strict UI
responsiveness during typing. Otherwise, implement after Phase 1-4 and use Phase 0 measurements to
decide.

### Design requirements

1. Run LMDB reads and expression evaluation off the GTK thread.
2. Give each filter request a monotonically increasing revision/token.
3. Cancel or ignore stale requests when newer filter text arrives.
4. Publish results back on the GTK/main runtime thread before mutating `SmartListSource` membership
   or notifying projections.
5. Confirm LMDB read transaction/thread constraints before sharing any library objects across
   threads.
6. Keep `TrackListProjection` notifications on the thread expected by GTK adapters, or dispatch them
   explicitly to the GTK main context.

### Possible shape

```text
TrackViewPage debounce
  └─ ViewService::setFilterAsync(viewId, expression, revision)
       ├─ stage/compile expression
       ├─ worker: evaluate source membership using read transaction
       └─ main context: apply membership if revision is still current
             └─ projection reset/update deltas
```

### Tests

- Stale result test: slow request A completes after faster request B and is ignored.
- Error test: invalid expression reports an error without clearing a newer valid result.
- Threading test or stress test: repeated filter changes during library mutations do not crash.

### Done criteria

- Filter typing remains responsive under the target library size.
- No stale result can overwrite a newer filter.
- GTK model updates still happen on the expected main context.

---

## 11. Updated Work Estimates

| Phase | Files touched | Estimated effort | Risk | Notes |
|---|---:|---:|---:|---|
| 0. Measurement | 3-6 | 0.5-1 day | Low | Required to validate claims. |
| 1. Runtime filter ownership/rebinding | 6-10 | 2-4 days | Medium/High | Projection replacement and ordering contract are correctness-sensitive. |
| 2. Filter responsiveness/status | 4-8 | 1-3 days | Medium | Debounce is easy; async support moves risk to Phase 6. |
| 3. O(1) selection/playing | 2-4 | 1-2 days | Medium | Avoid stale index bugs. |
| 4. Batch deltas | 2-4 | 1-2 days | Medium | Must preserve model/projection count invariant. |
| 5. Lazy model/memory | 5-10 | 4-8 days | High | Real fix requires model architecture work. |
| 6. Background evaluation | 5-10 | 4-8 days | High | Threading, cancellation, and LMDB constraints need careful review. |

---

## 12. Acceptance Targets

Use measured targets rather than fixed assumptions:

- `setPlayingTrackId()` should be independent of total visible rows in normal track-change cases.
- `selectTrack()` should be independent of total visible rows when the target is present in the
  projection.
- Projection reset should emit a small bounded number of GTK model changes, ideally one reset splice.
- Opening a large track page should not materialize one heavyweight row object per track after the
  lazy model phase.
- Filter input should either:
  - be explicitly documented as debounced but synchronously evaluated; or
  - be fully asynchronous with stale-result cancellation.

---

## 13. Summary

The corrected plan is feasible, but the safe path is narrower than the previous draft:

1. Fix projection lifetime/rebinding before routing GTK filters to runtime filtering.
2. Do not claim filter routing is non-blocking until evaluator work leaves the GTK thread or is
   proven fast enough with measurements.
3. Move filter error/status state out of `TrackListAdapter` and into runtime/page state.
4. Use projection indices for direct row targeting, but cache track IDs rather than stale row
   indices.
5. Batch projection deltas while preserving strict model/projection count invariants.
6. Treat memory reduction as a lazy model problem, not a provider-cache-only problem.
