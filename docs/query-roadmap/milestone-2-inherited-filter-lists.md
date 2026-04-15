# Milestone 2: Inherited-Filter Saved Lists

## Objective

Formalize the saved-list model that already exists in the code: users create lists with local filter expressions, and child smart lists inherit their source membership from the selected parent list.

This milestone turns current implementation behavior into an explicit product contract instead of leaving it as an accidental side effect of the load pipeline.

## Scope

In scope:

- user-facing language limited to filter expressions
- explicit parent, local, and effective filter semantics
- list-creation and preview UX aligned around inherited smart lists
- saved-list persistence that stores local filter text while preserving inherited runtime behavior

Out of scope:

- user-facing `group by` or `sort by` syntax
- dedicated albums or artists presenters
- virtual artist, album, or genre nodes in the left tree
- replacing inherited smart lists with non-inheriting saved filters

## Deliverables

1. A clearly documented model for local versus effective filter behavior.
2. A creation flow that makes inherited filtering understandable and predictable.
3. Saved-list persistence that stores local filter text and reloads inherited semantics consistently.
4. Aligned preview, save, and load behavior for child smart lists.

## Implementation Plan

### 1. Keep User Input Filter-Only

Treat the existing scalar expression language as the full user-facing input contract.

Requirements:

- normal UI copy should say `Filter` or `Expression`, not full query terminology
- keep bare predicates valid as the normal saved-smart-list syntax
- if internal `Query` objects remain, derive them from the effective filter rather than asking users to author them directly

### 2. Make Inheritance Explicit In The Creation Flow

The creation dialog should clearly communicate three layers:

- inherited filter from the parent list
- local filter entered for the new child
- effective filter produced by combining them

Key rule:

- a child smart list means "take my parent's membership, then filter it again"

### 3. Persist Local Filter, Not Flattened Effective Filter

Persisted smart-list behavior should be defined as follows:

- manual list: store explicit `trackIds`
- smart list: store only the local filter text plus the parent link

This preserves hierarchy meaning and keeps the tree editable. Flattening children into one effective filter would erase why the hierarchy exists.

### 4. Keep Preview, Save, And Load Aligned

All three paths should agree:

- preview uses the parent membership as its source
- saved child lists reload against the parent membership on startup
- errors are reported against the local filter but the user can still see the effective combination being applied

### 5. Minimize Migration Complexity

Because the feature is still pre-release, prefer one canonical inherited-list format.

Decision:

- do not carry a second non-inheriting smart-list mode unless a real product need appears
- reset development data if necessary rather than preserving abandoned semantics in the storage layer

## Code Areas Expected To Change

- `app/NewListDialog.*`
- `app/MainWindow.*`
- `app/model/ListDraft.*`
- `include/rs/core/List*`
- `src/core/List*`
- optionally `app/QueryExpressionBox.*` for naming or hint updates

## Risks And Decisions

### Risk: Inherited Semantics Stay Implicit And Confusing

Decision:

- keep the semantics, but make them visible in labels, preview, and documentation
- do not hide inheritance while still depending on it for actual runtime behavior

### Risk: Flattening Filters Makes Hierarchy Cosmetic

Decision:

- preserve parent-linked inheritance as a first-class concept
- a child list should continue to mean something narrower than its parent because of runtime source membership, not just because of tree placement

## Verification Steps

### Build

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build /tmp/build --parallel"
```

### Automated Checks

- add or update focused tests for saved-list round trips if a suitable test seam exists
- run:

```bash
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

### Manual Checks

1. Create a smart list under `All Tracks` and verify it behaves as a normal filter list.
2. Create a child smart list under an existing filtered parent and verify the child only sees the parent's membership.
3. Restart the app and verify the same child list still resolves against the parent rather than all tracks.
4. Edit or recreate a child list with an invalid local filter and verify the dialog still shows inherited and effective context clearly.

## Exit Criteria

- inherited smart-list behavior is documented and intentional
- creation, preview, save, and load all agree on parent-plus-local semantics
- hierarchy remains product-meaningful instead of cosmetic
