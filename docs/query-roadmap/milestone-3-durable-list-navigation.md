# Milestone 3: Durable List Navigation

## Objective

Make the left pane a stable hierarchy of named lists while keeping the right pane focused on the songs browser.

This milestone turns inherited smart lists into durable navigation, instead of relying on runtime grouping or additional presenters to help users rediscover the same content.

## Scope

In scope:

- `All Tracks` plus saved lists in the left tree
- hierarchy built from stored list relationships only
- selection behavior that binds the chosen list to the songs page
- a clear separation between durable navigation and temporary right-pane grouping
- parent-to-child source resolution for inherited smart lists

Out of scope:

- virtual `group by` nodes in the left tree
- builtin `Artists`, `Albums`, or `Genres` roots as prerequisites for release
- dedicated right-panel presenters beyond the songs page

## Deliverables

1. A navigation model that covers `All Tracks` and stored lists.
2. Tree-building logic based on saved hierarchy only.
3. Selection behavior that consistently loads the chosen list into the songs browser.
4. A page or view-context strategy that preserves inherited source semantics and playback behavior while keeping navigation simple.

## Navigation Principles

- left tree shows durable entry points only
- users build specialty navigation by saving filters they care about inside a meaningful hierarchy
- hierarchy is not just visual grouping; it defines the source membership for child smart lists
- right-pane grouping changes how the current list is viewed, not what nodes exist in the tree
- tree depth should stay intentionally shallow and music-library focused

## Implementation Plan

### 1. Keep The Tree Durable And Predictable

The initial tree should contain:

- builtin `All Tracks`
- stored manual lists
- stored filter-backed lists

Use saved `parentId` for both hierarchy and smart-list source resolution.

### 2. Resolve Selection To A Songs Browsing Context

Selecting a node should produce a complete songs-view context:

- page title
- track membership source
- local filter text within the page if supported
- local grouping state for browsing

For smart lists, that track membership source should come from the selected parent page when possible, matching the current load pipeline.

Implementation note:

- the current page-per-list strategy is acceptable if it stays simple
- if page proliferation becomes awkward, a reusable songs page bound to the selected context is also acceptable

### 3. Let Saved Filters Carry Discovery Work

Design the tree around the assumption that users will save filters for recurring destinations such as:

- favorite genres
- frequently played artists
- specific album collections
- years, moods, or tags

This reduces pressure to turn the tree into a runtime browser.

It also means the tree itself becomes a compact personal taxonomy: broad parent lists can be refined into narrower children without losing the parent's meaning.

### 4. Preserve Separation Of Concerns

Keep the responsibilities sharp:

- left tree chooses the durable list
- left tree hierarchy defines the inherited source for smart children
- right pane chooses how that list is viewed
- local grouping does not create or remove navigation nodes

## Code Areas Expected To Change

- `app/MainWindow.*`
- list tree row and node types under `app/`
- `app/TrackViewPage.*`
- optionally `app/model/` types used to describe a songs-view context

## Risks And Decisions

### Risk: Reintroducing Virtual Browse Nodes Through The Side Door

Decision:

- do not materialize artist, album, genre, or year groups as left-tree children in this roadmap
- keep runtime grouping inside the right pane

### Risk: Left Navigation And Right Browsing Drift Apart

Decision:

- every selected node must resolve to one clear songs browsing context and one clear playback stream
- parent-linked smart lists must resolve against the intended parent source, not accidentally against all tracks
- avoid separate notions of `selected list` and `active playback source`

## Verification Steps

### Build

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build /tmp/build --parallel"
```

### Automated Checks

- add focused tests for tree or context resolution if a suitable seam exists
- run:

```bash
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

### Manual Checks

1. Start the app and verify `All Tracks` plus saved lists appear consistently.
2. Create nested saved lists and verify hierarchy survives restart.
3. Select a child smart list and verify the right pane resolves membership from the parent list rather than the whole library.
4. Select different lists and verify the right pane always stays in songs mode.
5. Change the local group preset in the songs page and verify the left tree remains unchanged.

## Exit Criteria

- left navigation is stable and durable
- saved filters act as reliable navigation shortcuts with meaningful inherited hierarchy
- the right pane stays songs-first while still supporting browsing by grouping
