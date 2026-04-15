# Query And Navigation Roadmap

## Goal

Reach a built-in filter-driven library UI with:

- user-facing filter expressions for saved lists
- stable left-side navigation built from `All Tracks` and saved list hierarchy
- hierarchical saved lists whose effective membership inherits from parent lists
- a songs-first right pane with lightweight grouping for browsing by album, artist, genre, or year
- a single playback contract where every view resolves to a linear `TrackId` stream

## Product Direction

The application should stay a music player first.

- users write filters, not database-style full queries
- the left tree should show durable saved entry points, not volatile runtime result nodes
- saved-list hierarchy should be meaningful: a child list narrows its parent's membership
- the right pane is where browsing happens; the same songs page can be regrouped locally
- saved filters should help users build personal navigation for favorite genres, artists, albums, moods, and tags
- if richer query or planning types remain internally, they should stay implementation details rather than expand the normal user-facing surface area

## Target Architecture

### 1. Filter Language Layer

Keep the scalar expression language as the user-facing input contract.

Design notes:

- normal UI should speak in terms of `Filter` or `Expression`, not `Query`
- user-authored input should stay equivalent to a predicate or `where` clause only
- do not require user-authored `group by` or `sort by` syntax for everyday use
- internal query objects may still exist if they simplify implementation, but they should be derived from effective filter input rather than exposed as the primary product model

### 2. Saved List Layer

Saved lists should be explicit and easy to explain.

Target stored state:

- parent relationship
- display metadata such as name and description
- explicit list kind: manual membership or filter-backed
- local filter text for filter-backed lists
- manual track membership when applicable

Important semantic rules:

- parent-child hierarchy organizes navigation and defines inheritance for filter-backed lists
- the effective membership of a child smart list is `parent membership` filtered again by the child's local filter
- manual lists remain explicit membership snapshots

### 3. Navigation Layer

The left tree should represent durable entry points only.

Initial node families:

- builtin `All Tracks`
- stored manual lists
- stored filter-backed lists
- optional organizational folders later if the product needs them

Non-goals for the left tree:

- do not materialize runtime `group by` members under arbitrary nodes
- do not mirror the currently selected grouped result into the navigation tree
- do not require builtin `Artists` or `Albums` roots before the songs-first browsing model proves insufficient

### 4. Right Pane Layer

The right pane should stay songs-first.

Initial target behavior:

- one songs presenter built on a flat playback stream
- simple local grouping presets such as `None`, `Artist`, `Album`, `Album Artist`, `Genre`, and `Year`
- grouping remains a presentation choice, not part of saved-list identity
- grouping never replaces inherited list semantics; it only changes how the current list is viewed
- collapsible group sections are valuable follow-up once the grouped songs browser is stable

### 5. Playback Contract

For every visible songs view:

- define a canonical playback stream as ordered `TrackId`s
- make selection, activation, queueing, and export use that stream
- never let grouping or collapse state obscure the answer to "what plays next?"

## Current State To Evolve From

- [`TrackViewPage`](file:///home/rocklee/dev/RockStudio/app/TrackViewPage.cpp#L112-L303) already has a songs-table pipeline, local grouping controls, and GTK section headers.
- [`TrackPresentation`](file:///home/rocklee/dev/RockStudio/app/TrackPresentation.cpp#L139-L271) already defines music-oriented grouping and sort presets.
- [`NewListDialog`](file:///home/rocklee/dev/RockStudio/app/NewListDialog.cpp#L21-L44) already composes a child's effective expression as `parent and local`, and its preview pipeline uses the parent membership list as the source.
- [`MainWindow::buildPageForStoredList`](file:///home/rocklee/dev/RockStudio/app/MainWindow.cpp#L1033-L1075) already loads smart lists by filtering the parent page's membership list rather than always filtering all tracks.
- [`ListView`](file:///home/rocklee/dev/RockStudio/include/rs/core/ListView.h#L25-L32) and [`ListView::filter()`](file:///home/rocklee/dev/RockStudio/src/core/ListView.cpp#L46-L59) still describe the persisted list model in terms of local filter text, which matches the inherited-filter design.

## Milestones

| Milestone | Outcome | Depends On | Plan |
| --- | --- | --- | --- |
| 1 | Harden the current grouped songs browser foundation | none | [milestone-1-grouped-songs-mode.md](./milestone-1-grouped-songs-mode.md) |
| 2 | Inherited-filter saved lists and creation flow | 1 recommended | [milestone-2-inherited-filter-lists.md](./milestone-2-inherited-filter-lists.md) |
| 3 | Durable hierarchical list navigation and songs-page binding | 2 | [milestone-3-durable-list-navigation.md](./milestone-3-durable-list-navigation.md) |
| 4 | Collapse, polish, performance, and release validation | 1-3 | [milestone-4-polish-and-release-validation.md](./milestone-4-polish-and-release-validation.md) |

## Milestone Selection Rationale

This roadmap intentionally narrows the product surface before adding more browsing modes.

- it focuses on one strong songs browser instead of several half-finished presenters
- it keeps saved-list creation close to how hierarchical smart lists already work in the codebase
- it lets users build durable navigation by saving filters inside a meaningful inherited tree instead of learning query clauses
- it defers heavier browse specialization until grouped songs proves insufficient in real usage

## Cross-Cutting Rules

### User-Facing Simplicity

- users write only filters
- grouping and sort defaults should come from UI controls or code, not a required DSL
- prefer explicit labels like `Filter` and `Group` over query terminology in the normal flow

### List Semantics

- a saved list is either manual membership or a named filter
- smart-list parent-child links organize navigation and define inherited filtering
- child smart lists should keep storing local filter text even though their effective membership is inherited
- avoid introducing alternate semantics that would make hierarchy stop mattering

### Navigation Discipline

- left tree shows durable nodes only
- hierarchy should stay semantically meaningful because child lists derive from parent membership
- do not mirror runtime group sections into the tree
- if future builtin roots like `Artists` or `Albums` return, treat them as optional product additions rather than prerequisites for this roadmap

### Playback Contract

- visible order remains the canonical playback order
- grouping and collapse state must not break play, queue, or export behavior

### Performance

- keep grouped songs fast enough for large libraries
- cache row data and resolved labels where it lowers repeated GTK work
- optimize after the simpler end-to-end model is working, not before

## Common Verification Commands

Use these commands for milestones that modify code.

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build /tmp/build --parallel"
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

When a milestone touches core logic in `src/core`, `src/expr`, `src/tag`, or `include/rs`, at minimum complete the debug build before considering the milestone done.
