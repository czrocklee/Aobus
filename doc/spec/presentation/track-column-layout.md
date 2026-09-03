---
id: presentation.track-column-layout
type: spec
status: current
domain: presentation
summary: Defines shared track-table column alignment, sizing, resizing, visibility, persistence ownership, and desktop and TUI adaptation.
---
# Track-column layout specification

## Scope

This specification owns the platform-neutral policy for track-table column
alignment, sizing, visibility, ordering, and user resize behavior.
It defines start/end alignment, fixed and flexible column roles, minimums,
weighted allocation, canonical per-list frontend state, and terminal adaptation.

The presentation supplies the available fields and their initial semantic order.
A persisted layout may hide or reorder those fields without making a field outside
the active presentation available.
This specification does not own exact widget geometry, terminal hit testing, or
library data.
The [persisted presentation-state reference](../../reference/presentation/persisted-state.md) owns the versioned document.

## Code boundary

This contract spans the UIModel and frontend layers from the [system architecture](../../architecture/system-overview.md), as refined by the [presentation architecture](../../architecture/presentation.md).
Field sizing policy, the pure width solver, and the in-memory layout store live under `ao::uimodel` in `app/include/ao/uimodel/library/presentation/` and `app/uimodel/library/presentation/`.
They use abstract integer units and cannot depend on GTK, WinUI, or FTXUI.

`TrackColumnLayoutYamlSchema` lives beside that UIModel state and converts between the semantic map and a strict versioned persistence document.
It depends on runtime's stable track-field vocabulary but has no path or GTK dependency.

GTK adapts the policy in `app/linux-gtk/track/TrackColumnController` and persists per-list state through `GtkLayoutStateStore`.
WinUI adapts it in `app/windows-winui/track/TrackListController` and persists
per-list state through `LibrarySession`.
TUI adapts the same solver through `app/tui/TerminalTrackColumnLayout`, renders the result in `TrackTable`, and persists per-list canonical state through `TuiLayoutStateStore`. Its concrete widths are terminal cells rather than desktop pixels.

## Terminology

- A **fixed column** carries a bounded value and has a concrete width.
- A **flexible column** carries unbounded text and receives a weighted share of remaining width.
- **Start alignment** places text at the reading-direction start; **end alignment** places bounded scalar text at the reading-direction end.
- The **minimum width** is a hard lower bound independent of the preferred/default width.
- A **solve specification** is one field plus its fixed width or weight, default width, and minimum width.
- A **canonical state** is the normalized persisted representation of one column.
- A **geometry unit** is selected by the containing frontend document: desktop layout files use desktop geometry, while the TUI layout file uses terminal cells.

## Invariants

- Every visible column receives at least its normalized minimum width.
- Field-value alignment is one shared UIModel decision: bounded numeric, duration, technical scalar, timestamp, and display-track-number fields use end alignment; ordinary text and composite fields use start alignment.
- GTK, WinUI, and TUI translate the shared start/end value without maintaining frontend-specific field sets.
- Fixed widths are allocated before flexible widths.
- With at least one flexible column and sufficient space, solved widths sum to the viewport width.
- When fixed widths plus flexible minimums exceed the viewport, flexible columns remain at minimum and horizontal overflow is preserved.
- When all visible columns are fixed, trailing viewport space is not assigned to an arbitrary column.
- Flexible weights are positive and finite when converted to canonical state. Ordinary values are rounded to three decimal places, values below the smallest canonical step clamp to `0.001`, and extreme finite values remain finite rather than overflowing during rounding.
- Canonical flexible state stores `width = -1` and a positive `weight`; canonical fixed state stores a positive `width` and `weight = -1`.
- A missing, invalid, or pre-layout viewport uses preferred widths instead of producing zero-width columns.
- A resize never changes field identity, source membership, sort order, or grouping.
- Hidden columns retain their canonical order and sizing.
- GTK and WinUI visibility interaction keeps at least one field visible; TUI currently consumes persisted visibility but exposes no reorder/visibility editor.
- TUI fixed widths, preferred widths, minimums, pointer deltas, and viewport measurements are all terminal-cell counts. Restored fixed widths and explicit pointer targets are bounded to the supported 8 through 160 cells before solving. No pixel-to-cell conversion participates in restore or save.
- A TUI drag changes only preview state until normal pointer release commits one per-list canonical update.

## State model

`TrackColumnSolveSpec` is the pure solver input.
`TrackColumnState` is the canonical UI-local order, sizing, and visibility state
for a field.
`TrackColumnLayouts` maps `ListId` to an ordered vector of column states and emits the affected list id after a real change.
When constructed with `LibraryChanges`, it removes every layout named by a committed list deletion and emits each removed id only after the complete deletion set has been pruned.
A full library reset carries no incremental List ids, so it clears the complete layout map and emits every removed key.

That lifecycle only covers deletions the owner observed while it was alive.
`restore()` therefore takes the library's live list ids and drops every entry naming a list that no longer exists, so a document written before a deletion, or one whose cleanup write never landed, cannot reintroduce a layout that a reused list id would inherit.
A virtual list id references no user-created list and is retained without appearing among the live ids.

The invalid list id cannot receive an update.

## Commands and transitions

### Initial solve

The adapter combines visible runtime fields with stored state.
Fixed fields take a stored positive width when available; flexible fields take a stored positive weight.
Absent state falls back to field presentation policy.
Stored fields marked hidden are excluded from the solve while retaining their
canonical state for later restoration.

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

TUI begins a resize from the rendered header handle and previews the canonical candidate against the current list without mutating `TrackColumnLayouts`.
Pointer release recomputes the candidate at the release coordinate and performs one model update, which is the persistence trigger.
Opening or closing an overlay, entering text input, navigating to another list, receiving an unrelated pointer press, or frontend teardown cancels the gesture and discards the preview.
TUI clamps explicit pointer targets to 8 through 160 terminal cells before the shared representable-range rules are applied.

### User reorder and visibility

Desktop adapters may move an active presentation field left or right and may
toggle its visibility. Reordering changes only the stored field order.
Hiding a field retains its sizing state, and showing it restores the field into
that order. An adapter rejects an attempt to hide the last visible field.

## Failure and cancellation

Width operations are synchronous, deterministic value transformations and expose no recoverable error channel.
Size mismatches in conversion helpers preserve the prior solve specifications.
Unknown resize fields are no-ops.
TUI pointer cancellation is a frontend interaction boundary: an interrupted preview performs no model update and therefore no persistence write.

Persistence deserialization validates one complete layout group before replacing the caller's state.
An unsupported version, unknown or duplicate field, duplicate or invalid list
id, missing or malformed visibility, noncanonical dimensions, or structural
mismatch rejects the group and preserves the seeded state.
Persistence I/O failures belong to the [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) and do not change solver behavior.

## Persistence and versioning

Column state is UI-local per-library managed state keyed by list id.
GTK stores desktop geometry in `gtk_layout.yaml`; WinUI stores desktop geometry in `winui_layout.yaml`; TUI stores terminal geometry in `tui_layout.yaml`.
All three are per-library documents, so a list id keyed in one is only ever read back against the library that produced it.
The `trackView.columnLayouts` group carries required `version: 2` and stores
field id, canonical dimensions, and visibility.
The exact group shape is owned by the [persisted presentation-state reference](../../reference/presentation/persisted-state.md), while its containing document is registered by the [application managed-state reference](../../reference/persistence/application-config.md).

Loading uses strict recursive structure plus semantic validation and never installs a partial column vector.
Unversioned numeric layouts and unsupported versions are rejected without an automatic rewrite.
The explicit schema returns `NotSupported` for a future version before interpreting its layouts.
GTK suppresses layout/preference persistence callbacks while installing deserialized startup candidates, so a valid sibling group cannot rewrite a rejected layout merely because bulk state changed.
WinUI loads a complete typed candidate before installing it, once the library that gives its list ids meaning is open, and saves the group by itself from the store's persist port.
TUI loads each group independently before connecting save observers, and its single store writer saves column layouts and presentation preferences through one atomic `saveTogether()` candidate.
The solver owns no file format and accepts only deserialized `TrackColumnState` values.

Desktop and TUI documents share the semantic schema but not geometry units or writer authority.
A TUI process never reads `gtk_layout.yaml`, writes desktop widths, converts persisted pixels, or duplicates a cell-to-pixel representation.

## Frontend observations

GTK maps shared field alignment to its label alignment, assigns every `Gtk::ColumnViewColumn` a solved `fixed_width`, and does not use one expanding column as a substitute for the shared solver.
It resolves the viewport from the horizontal adjustment page size, with widget width as a pre-mapping fallback.

WinUI builds native header and row-cell controls from the active visible field
order. It solves against the current viewport, keeps header and rows on one
horizontal scroll surface, uses `ListView` for vertical recycling, and writes
resize, reorder, and visibility changes through the shared state.

TUI maps shared start/end alignment to terminal-cell alignment and supplies bounded terminal-cell defaults with an 8-cell minimum.
Projected fixed fields retain cell widths within the TUI adapter's supported 8-through-160-cell range; persisted flexible fields retain canonical weights and therefore reflow when the terminal viewport changes.
Persisted field order and visibility are merged with the active presentation before every solve.

## Implementation map

- [`TrackColumnDefaults.cpp`](../../../app/uimodel/library/presentation/TrackColumnDefaults.cpp) classifies field alignment and sizing and supplies defaults and minimums.
- [`TrackColumnWidthSolver.cpp`](../../../app/uimodel/library/presentation/TrackColumnWidthSolver.cpp) implements allocation, conversion, resize, and canonicalization.
- [`TrackColumnLayouts.cpp`](../../../app/uimodel/library/presentation/TrackColumnLayouts.cpp) owns per-list UI-local state, change signals, and stored-layout merging.
- [`TrackColumnLayoutYamlSchema.cpp`](../../../app/uimodel/library/presentation/TrackColumnLayoutYamlSchema.cpp) owns explicit YAML mapping, versioned persistence conversion, and validation.
- [`TrackColumnController.cpp`](../../../app/linux-gtk/track/TrackColumnController.cpp) adapts GTK viewport and drag events.
- [`TrackListController.cpp`](../../../app/windows-winui/track/TrackListController.cpp) adapts WinUI headers, rows, viewport, reordering, and visibility.
- [`TerminalTrackColumnLayout.cpp`](../../../app/tui/TerminalTrackColumnLayout.cpp) adapts shared state and the solver to terminal geometry; [`TrackTable.cpp`](../../../app/tui/TrackTable.cpp) renders the projected result; [`TuiLayoutStateStore.cpp`](../../../app/tui/TuiLayoutStateStore.cpp) owns the TUI file boundary.

## Test map

- [`TrackColumnDefaultsTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnDefaultsTest.cpp) protects field alignment, sizing roles, and default/minimum policy.
- [`TrackColumnWidthSolverTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnWidthSolverTest.cpp) protects distribution, overflow-safe finite-weight normalization, viewport overflow, convergence, resize, and canonical state.
- [`TrackColumnLayoutsTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnLayoutsTest.cpp) protects per-list state, cascade deletion, notification behavior, and deletion-callback lifetime.
- [`TrackColumnLayoutYamlSchemaTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnLayoutYamlSchemaTest.cpp) protects stable ids, canonical dimensions, and whole-object rejection.
- [`TrackColumnLayoutMergingTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackColumnLayoutMergingTest.cpp) protects stored visibility and presentation-field filtering.
- [`TrackColumnControllerTest.cpp`](../../../test/unit/linux-gtk/track/TrackColumnControllerTest.cpp) protects the GTK adapter.
- [`TerminalTrackColumnLayoutTest.cpp`](../../../test/unit/tui/TerminalTrackColumnLayoutTest.cpp), [`TrackTableTest.cpp`](../../../test/unit/tui/TrackTableTest.cpp), [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp), and [`TuiLayoutStateStoreTest.cpp`](../../../test/unit/tui/TuiLayoutStateStoreTest.cpp) protect terminal projection, rendering, commit/rollback gestures, and persistence.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Track-list presentation specification](track-presentation.md)
- [List presentation preference specification](list-preference.md)
- [Application managed-state reference](../../reference/persistence/application-config.md)
- [Persisted presentation state](../../reference/presentation/persisted-state.md)
