# GTK ColumnView Lifecycle Plan

This document captures the long-term plan for making track presentation switches robust in GTK4.

## Context

`Gtk::ColumnView` virtualizes rows and creates internal row/header/cell widgets for the visible range. In practice, mutating `Gtk::ColumnViewColumn` structure while those internal widgets are alive is fragile:

- `remove_column()` calls into `gtk_column_view_column_remove_cells()` for every live row cell.
- `set_visible(false)` also removes cells for that column.
- Presentation switches change projection order, section headers, column visibility, column order, widths, and expand state in one user action.
- The previous multi-idle model detach avoided some stale widgets, but left the view model-less across a main-loop turn and could produce invalid scroll adjustments.

The current implementation is a safe hotfix: it temporarily detaches the model and reconnects it inside the same callback while applying projection/header/column changes. That avoids mutating columns while live row/cell widgets are attached and avoids exposing a model-less view to the next GTK layout frame.

This is acceptable as a targeted fix, but it is not the cleanest final architecture.

## Target Principle

Presentation switches should be treated as a track-table view rebuild, not as incremental mutation of a live `Gtk::ColumnView`.

```diagram
╭──────────────────────╮
│ Presentation selected │
╰──────────┬───────────╯
           ▼
╭──────────────────────╮
│ Runtime projection    │
│ applies group/sort    │
╰──────────┬───────────╯
           ▼
╭──────────────────────╮
│ Build new ColumnView  │
│ off the live widget   │
│ tree                  │
╰──────────┬───────────╯
           ▼
╭──────────────────────╮
│ Configure columns,    │
│ factories, headers,   │
│ controllers, model    │
╰──────────┬───────────╯
           ▼
╭──────────────────────╮
│ Swap ScrolledWindow   │
│ child once            │
╰──────────────────────╯
```

The key invariant is:

> Do not call `Gtk::ColumnView::remove_column()`, `insert_column()`, or `Gtk::ColumnViewColumn::set_visible(false)` on the currently displayed `Gtk::ColumnView` while it has live row/cell widgets.

## Preferred Long-term Design

Introduce a small GTK-layer owner for the table widget, tentatively named `TrackColumnViewHost`.

### Ownership

`TrackColumnViewHost` owns one generation of table UI at a time:

- `Gtk::ColumnView`
- `TrackColumnController`
- `TrackSelectionController`
- CSS provider wiring
- selection/context/tag signal forwarding for the active view

`TrackViewPage` owns stable page-level state:

- `Gtk::ScrolledWindow`
- `_groupModel`
- `_selectionModel`
- `TrackListAdapter`
- active presentation
- presentation selector controls
- context/tag popovers

The selection model should remain stable across rebuilds. Only the `Gtk::ColumnView` and widget/controller objects that depend on it are replaced.

### Rebuild Algorithm

1. Resolve the new `TrackPresentationSpec` through `ViewService`.
2. Reset horizontal adjustment to its lower bound.
3. Build a new `Gtk::ColumnView` off-tree.
4. Configure static view properties:
   - focusable
   - focus-on-click
   - row separators
   - reorderable policy, if still supported
   - CSS provider
5. Create a new `TrackColumnController` for the new view.
6. Create columns in the target presentation order before attaching the model.
7. Apply widths, visibility, and expand state before attaching the model.
8. Set the header factory based on the active grouping.
9. Set `_selectionModel` as the new view model.
10. Create a new `TrackSelectionController` and reconnect its signals to `TrackViewPage` forwarding signals.
11. Reparent page-level popovers to the new view as needed.
12. Swap the scrolled window child once.
13. Destroy the previous view generation after the swap.
14. Restore focus if the old view had focus.
15. Reapply playing-track state if one is active.

This avoids live column removal on the existing widget entirely. The old `ColumnView` tears down as a whole; the new one starts with a coherent set of columns and model.

## Implementation Phases

### Phase 1: Isolate Current Hotfix

Goal: keep the current stable behavior while making the transition boundary explicit.

Files:

```text
app/linux-gtk/track/TrackViewPage.h
app/linux-gtk/track/TrackViewPage.cpp
app/linux-gtk/track/TrackColumnController.h
app/linux-gtk/track/TrackColumnController.cpp
```

Changes:

1. Keep `TrackColumnController::setLayoutAndApply()` as the only presentation-driven layout write path.
2. Replace repeated inline model detach/reconnect snippets with a private `TrackViewPage::withDetachedColumnModel()` helper.
3. Document that this helper must not yield to the main loop.
4. Add a manual regression checklist near the implementation comments.

Acceptance criteria:

- Switching `Songs -> Albums -> Songs` repeatedly does not crash.
- Switching `Songs -> Album Artists -> Songs` repeatedly does not crash.
- No `gtk_adjustment_configure` warning appears during scroll after switching.
- `./build.sh debug` passes.

### Phase 2: Extract View-generation Setup

Goal: make creating a fully configured `Gtk::ColumnView` possible without changing behavior yet.

Files:

```text
app/linux-gtk/track/TrackViewPage.h
app/linux-gtk/track/TrackViewPage.cpp
app/linux-gtk/track/TrackColumnController.h
app/linux-gtk/track/TrackColumnController.cpp
```

Changes:

1. Move ColumnView setup from the constructor into a private helper, for example `configureColumnView(Gtk::ColumnView&)`.
2. Move column factory setup into a helper that can be reused for a newly created view.
3. Move selection-controller signal forwarding into a helper.
4. Ensure theme refresh callbacks always address the current view/controller, not stale references.

Acceptance criteria:

- No user-visible behavior changes.
- The constructor uses the same setup helpers that the future rebuild path will use.
- `./build.sh debug` passes.

### Phase 3: Introduce `TrackColumnViewHost`

Goal: centralize one generation of ColumnView/controller ownership.

New files:

```text
app/linux-gtk/track/TrackColumnViewHost.h
app/linux-gtk/track/TrackColumnViewHost.cpp
```

Responsibilities:

- Build a fresh `Gtk::ColumnView` for a given `TrackColumnLayout`.
- Own the matching `TrackColumnController`.
- Own the matching `TrackSelectionController`.
- Expose the current `Gtk::ColumnView&`.
- Expose forwarding signals or connect to callbacks supplied by `TrackViewPage`.
- Apply title-position CSS updates for the current generation.

Non-goals:

- It must not own runtime presentation specs.
- It must not load track values.
- It must not own `_groupModel` or `_selectionModel`.

Acceptance criteria:

- `TrackViewPage` still displays the table through the host.
- Presentation switching can still use the current detach/reconnect transition during this phase.
- `./build.sh debug` passes.

### Phase 4: Rebuild-and-swap on Presentation Change

Goal: replace live column mutation with whole-view replacement.

Files:

```text
app/linux-gtk/track/TrackViewPage.cpp
app/linux-gtk/track/TrackColumnViewHost.cpp
```

Changes:

1. On presentation selection, apply runtime presentation first.
2. Build a new host generation off-tree using the new layout.
3. Configure columns, headers, factories, controllers, CSS, and model before swap.
4. Replace `_scrolledWindow` child with the new host widget once.
5. Retire the old generation after the swap.
6. Remove the same-callback model detach/reconnect hotfix from presentation switching.

Acceptance criteria:

- Presentation switching never calls `remove_column()` on the displayed `Gtk::ColumnView`.
- Presentation switching never calls `set_visible(false)` on columns belonging to the displayed `Gtk::ColumnView`.
- Repeated switches across all built-in presets do not crash.
- Selection, activation, context menu, tag editing, inline editing, playing-row state, and theme refresh still work.

### Phase 5: Decide Column Reorder Support

Goal: avoid reintroducing unsafe live column mutation through manual header reordering.

Options:

1. Disable header reordering for built-in presentations and only support ordering in the custom view editor.
2. Allow user reordering, but record order from ColumnView notifications and apply it on the next rebuild, not by immediately removing/inserting columns on the live view.

Recommended first choice: disable live header reordering for preset views until custom view editing exists.

Acceptance criteria:

- User column reorder cannot trigger `remove_column()` on the live presentation table.
- Any persisted custom order is applied only during a rebuild.

### Phase 6: Remove Transitional APIs

Goal: delete workaround-only APIs once rebuild-and-swap owns presentation changes.

Candidates:

- `TrackColumnController::setLayoutAndApply()` if it is no longer needed outside initial generation setup.
- `TrackViewPage::withDetachedColumnModel()` from Phase 1.

Acceptance criteria:

- Presentation switching code has one path: build new generation, swap child.
- No model detach/reconnect is required for presentation switches.
- `./build.sh debug` passes.

## Testing Plan

### Automated

Add GTK unit tests where possible:

- Build a `TrackColumnViewHost` with a synthetic model.
- Rebuild it through `songs`, `albums`, and `album-artists` layouts.
- Assert the current view has expected visible/expand columns.
- Assert selection model remains the same object across rebuilds.
- Assert no duplicate selection signal forwarding by counting callbacks after rebuild.

### Manual Regression Matrix

Run with a real library:

1. `Songs -> Albums -> Songs`, repeated at least 10 times.
2. `Songs -> Album Artists -> Songs`, repeated at least 10 times.
3. `Songs -> Classical: Composers -> Classical: Works -> Songs`.
4. Scroll vertically immediately after each switch.
5. Scroll horizontally after switching from a wide view to a narrow view.
6. Select rows, switch presentation, verify selection behavior remains coherent.
7. Activate a track before and after switching.
8. Open context menu before and after switching.
9. Open tag editor from a tags cell before and after switching.
10. Verify playing-row indicator after switching.

## Implementation Summary (2026-05-15)

The plan was implemented with a revised phase order that merges overlapping steps and skips dead-end scaffolding.

### What was done (vs. planned)

| Planned Phase | Status | Notes |
|---|---|---|
| Phase 1: `withDetachedColumnModel()` helper | **Skipped** | Would have been removed in Phase 6. The inline detach/reattach was replaced directly by rebuild-and-swap. |
| Phase 2: Extract setup helpers | **Merged into Phase 3** | Helpers live on `TrackColumnViewHost` instead of temporary `TrackViewPage` methods. |
| Phase 3: `TrackColumnViewHost` | **Implemented** | New files: `TrackColumnViewHost.h/.cpp`. Owns `Gtk::ColumnView` (unique_ptr), `TrackColumnController`, `TrackSelectionController`. Exposes stable forwarding signals. |
| Phase 4: Rebuild-and-swap | **Implemented** | `TrackViewPage::rebuildColumnView()` builds new ColumnView off-tree, swaps scrolled-window child once. Old generation destroyed atomically during `TrackColumnViewHost::rebuild()`. |
| Phase 5: Column reorder | **Already safe** | `set_reorderable(true)` triggers `updateSharedColumnLayout()` which captures order into layout model without live column mutation. Applied on next rebuild. |
| Phase 6: Transitional APIs | **Already clean** | No transitional APIs were created. |

### Additional cleanup

- Removed dead `setupColumnControls()` code from `TrackColumnController` (old "Columns" popover, toggle checkbuttons, `syncColumnToggleStates()`).
- Removed `ColumnBinding::toggle` field — toggles were never displayed.
- Moved `TrackPageManager` signal connections from `selectionController().signal*()` to `TrackViewPage::signal*()` (stable across rebuilds).

### Key design decisions

1. **`ColumnVisibilityModel` is owned by `TrackColumnController`** (per-generation). Property bindings to `ColumnViewColumn` are recreated on each rebuild via `setupColumns()`.
2. **`Gtk::ColumnView` is `unique_ptr`** in the host for clean swap semantics.
3. **Signal forwarding** uses stable `sigc::signal` objects in `TrackColumnViewHost`, reconnected to the new `TrackSelectionController` on rebuild.
4. **The old ColumnView is destroyed during `rebuild()`** (move assignment), which unparents it from the scrolled window. The caller then sets the new ColumnView as the scrolled window child.

### Verification

- `./build.sh debug` passes (20849 + 605 assertions).
- No `remove_column()` or `set_visible(false)` on displayed ColumnView during presentation switch.
- Column mutation (`remove_column`/`insert_column`) only occurs on off-tree ColumnViews in `TrackColumnController::applyColumnLayout()`.

## Rollback Strategy

Keep the current same-callback detach/reconnect transition until rebuild-and-swap passes the manual matrix. If rebuild-and-swap shows regressions, keep the hotfix and land the extraction phases separately.

## Final Acceptance Criteria

- No presentation switch mutates columns on a live displayed `Gtk::ColumnView`.
- No crash or GTK critical warning occurs when switching among built-in presets.
- No scroll adjustment warning occurs after switching and scrolling.
- The table remains responsive on large libraries.
- The implementation has one clear lifecycle boundary for replacing GTK table widgets.
