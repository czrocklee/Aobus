# Milestone 4: Collapse, Polish, And Release Validation

## Objective

Harden the songs-first browsing model for real users: add collapsible grouped sections, polish the UI, verify performance, and document the finished workflow.

## Scope

In scope:

- collapsible group sections in the songs browser
- lightweight group-level actions if they improve browsing materially
- performance tuning and caching follow-up
- release-level validation and documentation

Out of scope:

- new user-facing query syntax
- dedicated albums or artists presenters
- virtual group nodes in the left tree

## Deliverables

1. Collapsible grouped sections in the songs page.
2. Polished section headers, counts, and empty or error states.
3. Performance fixes for large libraries and repeated navigation.
4. Updated developer and user-facing documentation for the simplified model.

## Implementation Plan

### 1. Add Explicit Collapse State

Introduce state for which visible groups are expanded or collapsed.

Requirements:

- collapsing a group hides its member rows predictably
- expanding restores the same visible ordering
- collapse state should not corrupt selection, activation, or playback semantics

### 2. Keep Group Header Actions Lightweight

Candidates:

- expand or collapse
- play group
- queue group

Keep the control surface intentionally small so grouped songs still feels like a music browser rather than a file manager.

### 3. Performance Pass

Review the hotspots introduced by filter-backed lists, grouping, and collapse.

Areas to evaluate:

- row DTO loading breadth
- repeated label and key resolution churn
- regroup and collapse latency on large lists
- repeated page switches across inherited saved filters

### 4. Documentation And Release Sweep

Update documentation to explain the final product model:

- users save local filters to build navigation
- the left tree stores durable lists whose smart children inherit from parents
- the right pane stays songs-first and can be regrouped locally

Then run a final release-readiness sweep over the common flows.

## Risks And Decisions

### Risk: Collapse Logic Complicates The Model Too Much

Decision:

- keep collapse strictly one-level and tied to existing group sections
- do not let it become a generic tree-in-table system

### Risk: Polish Turns Back Into Feature Creep

Decision:

- prioritize rough edges that show up repeatedly in everyday browsing
- defer new product ideas unless they unblock release readiness directly

## Verification Steps

### Build

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build /tmp/build --parallel"
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

### Manual Regression Sweep

1. Open `All Tracks`, group by `Album`, collapse and expand several sections, and verify playback still starts from the expected row.
2. Open a saved filter list, group by `Artist`, and verify collapse state does not alter the underlying playback order unexpectedly.
3. Open a child smart list and verify collapse plus grouping still operate on the inherited membership only.
4. Switch repeatedly between saved lists and verify grouping plus collapse stay responsive.
5. Verify empty metadata cases still fall into predictable groups and can be collapsed safely.
6. Restart the app and verify saved lists plus songs browsing still behave consistently.

### Large-Library Checks

1. Measure startup impact from rebuilding the saved-list tree.
2. Measure grouping and collapse responsiveness on large libraries.
3. Measure repeated switching between several saved filter lists.

## Exit Criteria

- grouped songs browsing feels complete enough for release
- inherited saved-list navigation and songs playback flows are smooth on realistic libraries
- the simplified architecture is documented clearly enough for further iteration
