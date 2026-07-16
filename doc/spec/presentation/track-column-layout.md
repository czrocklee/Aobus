---
id: presentation.track-column-layout
type: spec
status: current
domain: presentation
summary: Defines shared track-table column roles, width solving, resizing, persistence ownership, and GTK and TUI adaptation.
---
# Track-column layout specification

## Scope

This specification owns the platform-neutral policy for track-table column sizing and user resize behavior.
It defines fixed and flexible column roles, minimums, weighted allocation, canonical resize state, per-list GTK persistence, and terminal adaptation.

It does not choose which fields are visible or their semantic order; those are part of the [track-list presentation specification](track-presentation.md).
It also does not own the exact serialized GTK layout fields, widget geometry, terminal hit testing, or library data.
The [persisted presentation-state reference](../../reference/presentation/persisted-state.md) owns the versioned document.

## Code boundary

This contract spans the UIModel and frontend layers from the [system architecture](../../architecture/system-overview.md), as refined by the [presentation architecture](../../architecture/presentation.md).
Field sizing policy, the pure width solver, and the in-memory layout store live under `ao::uimodel` in `app/include/ao/uimodel/library/presentation/` and `app/uimodel/library/presentation/`.
They use abstract integer units and cannot depend on GTK or FTXUI.

`TrackColumnLayoutCodec` lives beside that UIModel state and converts between the semantic map and a strict versioned persistence document.
It depends on runtime's stable track-field vocabulary but has no path or GTK dependency.

GTK adapts the policy in `app/linux-gtk/track/TrackColumnController` and persists per-list state through `GtkLayoutStateStore`.
TUI adapts the same solver in `app/tui/TrackTable` using terminal-column units and keeps manual overrides in the current TUI session.

## Terminology

- A **fixed column** carries a bounded value and has a concrete width.
- A **flexible column** carries unbounded text and receives a weighted share of remaining width.
- The **minimum width** is a hard lower bound independent of the preferred/default width.
- A **solve specification** is one field plus its fixed width or weight, default width, and minimum width.
- A **canonical state** is the normalized persisted representation of one column.

## Invariants

- Every visible column receives at least its normalized minimum width.
- Fixed widths are allocated before flexible widths.
- With at least one flexible column and sufficient space, solved widths sum to the viewport width.
- When fixed widths plus flexible minimums exceed the viewport, flexible columns remain at minimum and horizontal overflow is preserved.
- When all visible columns are fixed, trailing viewport space is not assigned to an arbitrary column.
- Flexible weights are positive and rounded to three decimal places when converted to canonical state.
- Canonical flexible state stores `width = -1` and a positive `weight`; canonical fixed state stores a positive `width` and `weight = -1`.
- A missing, invalid, or pre-layout viewport uses preferred widths instead of producing zero-width columns.
- A resize never changes field identity, source membership, sort order, or grouping.

## State model

`TrackColumnSolveSpec` is the pure solver input.
`TrackColumnState` is the canonical UI-local state for a field.
`TrackColumnLayoutStore` maps `ListId` to an ordered vector of column states and emits the affected list id after a real change.

The active list id is used only to expose the active field order.
It is not a second owner of the runtime view or list selection.
The invalid list id cannot receive an update.

## Commands and transitions

### Initial solve

The adapter combines visible runtime fields with stored state.
Fixed fields take a stored positive width when available; flexible fields take a stored positive weight.
Absent state falls back to field presentation policy.

The solver allocates fixed widths, reserves flexible minimums, and distributes the remaining width by normalized weight.
Flexible shares that fall below their minimum are pinned and the rest is redistributed until all active shares are representable.

### User resize

Resizing a flexible column changes the relative flexible widths.
The requested delta is absorbed by flexible columns to the right first and then those to the left, without taking any flexible column below its minimum.
The resized width is clamped to the representable range when other flexible columns must retain their minimums.

Resizing a fixed column stores a fixed width.
It may create overflow rather than forcing unrelated flexible columns below their lower bounds.

After solving, widths are converted back into canonical fixed widths and flexible weights.
Repeated solve/store cycles converge and avoid spurious changes caused only by floating-point noise.

## Failure and cancellation

Width operations are synchronous, deterministic value transformations and expose no recoverable error or cancellation channel.
Size mismatches in conversion helpers preserve the prior solve specifications.
Unknown resize fields are no-ops.

Persistence decoding validates one complete layout group before replacing the caller's state.
An unsupported version, unknown or duplicate field, duplicate or invalid list id, noncanonical dimensions, or structural mismatch rejects the group and preserves the seeded state.
Persistence I/O failures belong to the [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) and do not change solver behavior.

## Persistence and versioning

GTK column state is UI-local per-library managed state in `gtk_layout.yaml`, keyed by list id.
The `trackView.columnLayouts` group carries required `version: 1` and stores fields by stable textual id.
The exact group shape is owned by the [persisted presentation-state reference](../../reference/presentation/persisted-state.md), while its containing document is registered by the [application managed-state reference](../../reference/persistence/application-config.md).

Loading uses strict recursive structure plus semantic validation and never installs a partial column vector.
Unversioned numeric layouts and unsupported versions are rejected without an automatic rewrite.
GTK suppresses layout/preference persistence callbacks while installing decoded startup candidates, so a valid sibling group cannot rewrite a rejected layout merely because bulk state changed.
The solver owns no file format and accepts only decoded `TrackColumnState` values.

TUI manual column-width overrides are session-local terminal-column values and are not written to the GTK document.
Pixel and terminal-column states are not interchangeable persisted representations.

## Frontend observations

GTK assigns every `Gtk::ColumnViewColumn` a solved `fixed_width` and does not use one expanding column as a substitute for the shared solver.
It resolves the viewport from the horizontal adjustment page size, with widget width as a pre-mapping fallback.

TUI converts the shared field policy to bounded terminal-column defaults and minimums.
A manually overridden TUI column is treated as fixed for the current solve while remaining text columns continue to flex.

## Implementation map

- [`TrackFieldPresentationPolicy.cpp`](../../../app/uimodel/library/presentation/TrackFieldPresentationPolicy.cpp) classifies fields and supplies defaults and minimums.
- [`TrackColumnWidthSolver.cpp`](../../../app/uimodel/library/presentation/TrackColumnWidthSolver.cpp) implements allocation, conversion, resize, and canonicalization.
- [`TrackColumnLayoutStore.cpp`](../../../app/uimodel/library/presentation/TrackColumnLayoutStore.cpp) owns per-list UI-local state and change signals.
- [`TrackColumnLayoutCodec.cpp`](../../../app/uimodel/library/presentation/TrackColumnLayoutCodec.cpp) owns the versioned persistence conversion and validation.
- [`TrackColumnController.cpp`](../../../app/linux-gtk/track/TrackColumnController.cpp) adapts GTK viewport and drag events.
- [`TrackTable.cpp`](../../../app/tui/TrackTable.cpp) adapts the solver to terminal geometry.

## Test map

- [`TrackFieldPresentationPolicyTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackFieldPresentationPolicyTest.cpp) protects field roles and default/minimum policy.
- [`TrackColumnWidthSolverTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnWidthSolverTest.cpp) protects distribution, overflow, convergence, resize, and canonical state.
- [`TrackColumnLayoutStoreTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnLayoutStoreTest.cpp) protects per-list state and notification behavior.
- [`TrackColumnLayoutCodecTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnLayoutCodecTest.cpp) protects stable ids, canonical dimensions, and whole-object rejection.
- [`TrackColumnControllerTest.cpp`](../../../test/unit/linux-gtk/track/TrackColumnControllerTest.cpp) protects the GTK adapter.
- [`TrackTableTest.cpp`](../../../test/unit/tui/TrackTableTest.cpp) and [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp) protect terminal layout and resize gestures.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Track-list presentation specification](track-presentation.md)
- [List presentation preference specification](list-preference.md)
- [Application managed-state reference](../../reference/persistence/application-config.md)
- [Persisted presentation state](../../reference/presentation/persisted-state.md)
