---
id: presentation.track-column-layout
---

# Track-column layout specification

## Scope

This specification owns the platform-neutral policy for track-table column
alignment, sizing, visibility, ordering, and user resize behavior.
It defines start/end alignment, fixed and flexible column roles, minimums,
weighted allocation, canonical per-list frontend state, and terminal adaptation.

The authored presentation supplies the default columns and their initial
semantic order. It may also declare a field redundant with a group heading;
if the heading presents the same fact, that field is available to show but is
hidden by default unless the authored field list already includes it.
A persisted layout can show, hide, or reorder available fields without making
unrelated fields available.
This specification does not own exact widget geometry, terminal hit testing, or
library data.
The [persisted presentation-state reference](../../reference/presentation/persisted-state.md) owns the versioned document.

## Code boundary

This contract spans the UIModel and frontend layers from the [system architecture](../overview.md), as refined by the [presentation architecture](README.md).
Field sizing policy, the pure width solver, and the in-memory layout store live under `ao::uimodel` in `app/include/ao/uimodel/library/presentation/` and `app/uimodel/library/presentation/`.
They use abstract integer units and cannot depend on GTK, WinUI, or FTXUI.

`TrackColumnLayoutYamlSchema` lives beside that UIModel state and converts between the semantic map and a strict versioned persistence document.
It depends on runtime's stable track-field vocabulary but has no path or GTK dependency.

GTK adapts the policy in `app/linux-gtk/track/TrackColumnController` and persists per-list state through `GtkLayoutStateStore`.
WinUI adapts it in `app/windows-winui/track/TrackListController` and persists
per-list state through `LibrarySession`.
TUI adapts the same solver through `app/tui/TerminalTrackColumnLayout`, renders the result in `TrackTable`, and persists per-list canonical state through `LayoutStateStore`. Its concrete widths are terminal cells rather than desktop pixels.

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
- Canonical flexible state stores `width = -1` and a positive `weight`; canonical fixed state stores a positive `width` and `weight = -1`. The persisted form must also agree with the field's fixed or flexible sizing role; a canonical-looking form for the wrong role is invalid.
- A missing, invalid, or pre-layout viewport uses preferred widths instead of producing zero-width columns.
- A resize never changes field identity, source membership, sort order, or grouping.
- Hidden columns retain their canonical order and sizing.
- A visibility-only change does not establish sizing or order. Geometry changes preserve each field's visibility choice; a field without an explicit choice inherits the presentation default.
- WinUI's Columns command keeps at least one field visible. GTK and TUI consume persisted per-list visibility but do not expose per-list visibility or reorder commands. GTK's custom presentation editor changes the presentation's field list instead.
- TUI fixed widths, preferred widths, minimums, pointer deltas, and viewport measurements are all terminal-cell counts. Restored fixed widths and explicit pointer targets are bounded to the supported 8 through 160 cells before solving. No pixel-to-cell conversion participates in restore or save.
- A TUI drag changes only preview state until normal pointer release commits one per-list canonical update.

## State model

`TrackColumnSolveSpec` is the pure solver input.
`TrackColumnState` holds canonical sizing and order, an explicit visibility
choice, or both. An entry without geometry supplies no position; only entries
with geometry participate in saved order. Visibility can inherit the
presentation default or record an explicit show or hide. These choices do not
change which fields the active presentation makes available.
`TrackColumnLayouts` maps `ListId` to an ordered vector of column states and emits the affected list id after a real change.
When constructed with `LibraryChanges`, it removes every layout named by a committed list deletion and emits each removed id only after the complete deletion set has been pruned.
A full library reset carries no incremental List ids, so it clears the complete layout map and emits every removed key.

That lifecycle only covers deletions the owner observed while it was alive.
`restore()` therefore takes the library's live list ids and drops every entry naming a list that no longer exists, so a document written before a deletion, or one whose cleanup write never landed, keeps no layout for a list the library no longer has.
A list id is never reused within one database, so pruning also retires every entry for a list that a restore replaced.
The document lives outside the database, though, and can meet a rebuilt or replaced one that numbers its lists afresh, so it records the id of the library it was saved for; loading it for another library keeps only its virtual-list entries.
A virtual list id references no user-created list and is retained without appearing among the live ids.

The invalid list id cannot receive an update.

## Commands and transitions

### Initial solve

GTK, WinUI, and TUI use the authored view spec, not the projection's inferred
redundant metadata, to resolve available and default columns. Inherited
visibility follows the presentation. A List's explicit show or hide takes
precedence in every presentation that offers the field. An override for an
unavailable field is retained but does not make that field available.

The solver uses the resulting visible fields in the List's stored order,
followed by fields without stored positions in presentation order. Fixed fields take a
stored positive width when available; flexible fields take a stored positive
weight. Absent sizing falls back to field presentation policy.

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

WinUI can move a visible field left or right and can toggle any available
field, including one omitted by default because it is in a group heading. In
Albums, **Album** starts unchecked; checking it shows the album name both in
the group heading and in each track row. Reordering changes only the stored
field order. Hiding or showing a field retains any existing dimensions and
position. If the field has no saved position, a toggle does not create one;
resize or reorder can establish it. The choice persists when the List is reopened. Checking
a column again records an explicit show even if the current presentation
would show it by default; it does not reset the choice. WinUI rejects an
attempt to hide the last visible field.

## Failure and cancellation

Width operations are synchronous, deterministic value transformations and expose no recoverable error channel.
Size mismatches in conversion helpers preserve the prior solve specifications.
Unknown resize fields are no-ops.
TUI pointer cancellation is a frontend interaction boundary: an interrupted preview performs no model update and therefore no persistence write.

Persistence deserialization validates one complete layout group before replacing the caller's state.
An unsupported version, missing or malformed library id, unknown or duplicate
field, duplicate or invalid list id, missing or malformed visibility, dimensions that are noncanonical or do not
match the field's sizing role, or structural mismatch rejects the group and
preserves the seeded state.
Persistence I/O failures belong to the [persistence and managed-state architecture](../persistence/README.md) and do not change solver behavior.

## Persistence and versioning

Column state is UI-local per-library managed state keyed by list id.
GTK stores desktop geometry in `gtk_layout.yaml`; WinUI stores desktop geometry in `winui_layout.yaml`; TUI stores terminal geometry in `tui_layout.yaml`.
All three are per-library documents that record the id of the library they were saved for, so a list id keyed in one is never read back against another library's lists.
The `trackView.columnLayouts` group requires `version: 1` and `libraryId`.
The writer stores the field id and a `visibility` choice of `default`, `show`, or `hide`.
It includes canonical dimensions only for positioned columns.
The exact group shape is owned by the [persisted presentation-state reference](../../reference/presentation/persisted-state.md), while its containing document is registered by the [application managed-state reference](../../reference/persistence/application-config.md).

Loading uses strict recursive structure plus semantic validation and never installs a partial column vector.
Version 1 also accepts the earlier boolean `visible` member in place of `visibility`: `true` inherits the presentation default and `false` explicitly hides the field. Either form preserves the stored order and dimensions. Loading does not rewrite the group.
Unversioned layouts are rejected. The schema returns `NotSupported` for versions other than 1 before interpreting their layouts.
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
- [`TrackColumnLayouts.cpp`](../../../app/uimodel/library/presentation/TrackColumnLayouts.cpp) owns per-list UI-local state, resolves field availability and visibility against the authored presentation, and merges stored order with the current fields.
- [`TrackColumnLayoutYamlSchema.cpp`](../../../app/uimodel/library/presentation/TrackColumnLayoutYamlSchema.cpp) owns YAML mapping and validation.
- [`TrackPageHost.cpp`](../../../app/linux-gtk/track/TrackPageHost.cpp) supplies the authored presentation when a GTK page opens or its projection is replaced. [`TrackViewPage.cpp`](../../../app/linux-gtk/track/TrackViewPage.cpp) resolves the visible fields before attaching the model; [`TrackColumnController.cpp`](../../../app/linux-gtk/track/TrackColumnController.cpp) applies that order and visibility, solves desktop widths, and observes native width changes. [`GtkLayoutStateStore.cpp`](../../../app/linux-gtk/app/GtkLayoutStateStore.cpp) owns the GTK file boundary.
- [`TrackListController.cpp`](../../../app/windows-winui/track/TrackListController.cpp) adapts WinUI headers, rows, viewport, reordering, and visibility; [`LibrarySession.cpp`](../../../app/windows-winui/app/LibrarySession.cpp) owns the WinUI file boundary.
- [`TerminalTrackColumnLayout.cpp`](../../../app/tui/TerminalTrackColumnLayout.cpp) adapts shared state and the solver to terminal geometry; [`TrackTable.cpp`](../../../app/tui/TrackTable.cpp) renders the projected result; [`LayoutStateStore.cpp`](../../../app/tui/LayoutStateStore.cpp) owns the TUI file boundary.

## Test map

- [`TrackColumnDefaultsTest.cpp`](../../../test/uimodel/library/presentation/TrackColumnDefaultsTest.cpp) protects field alignment, sizing roles, and default/minimum policy.
- [`TrackColumnWidthSolverTest.cpp`](../../../test/uimodel/library/presentation/TrackColumnWidthSolverTest.cpp) protects distribution, overflow-safe finite-weight normalization, viewport overflow, convergence, resize, and canonical state.
- [`TrackColumnLayoutsTest.cpp`](../../../test/uimodel/library/presentation/TrackColumnLayoutsTest.cpp) protects per-list state, cascade deletion, notification behavior, and deletion-callback lifetime.
- [`TrackColumnLayoutYamlSchemaTest.cpp`](../../../test/uimodel/library/presentation/TrackColumnLayoutYamlSchemaTest.cpp) protects stable ids, canonical dimensions, and whole-object rejection.
- [`TrackColumnLayoutMergingTest.cpp`](../../../test/uimodel/library/presentation/TrackColumnLayoutMergingTest.cpp) protects stored visibility and presentation-field filtering.
- [`TrackColumnControllerTest.cpp`](../../../test/linux-gtk/track/TrackColumnControllerTest.cpp) checks native GTK layout application and resize. [`TrackViewPageTest.cpp`](../../../test/linux-gtk/track/TrackViewPageTest.cpp) checks stored visibility across GTK page rebuilds. [`TrackPageHostTest.cpp`](../../../test/linux-gtk/track/TrackPageHostTest.cpp) checks that replacing the projection does not make an inferred redundant field available.
- [`TerminalTrackColumnLayoutTest.cpp`](../../../test/tui/TerminalTrackColumnLayoutTest.cpp), [`TrackTableTest.cpp`](../../../test/tui/TrackTableTest.cpp), [`EventControllerTest.cpp`](../../../test/tui/EventControllerTest.cpp), and [`LayoutStateStoreTest.cpp`](../../../test/tui/LayoutStateStoreTest.cpp) protect terminal projection, rendering, commit/rollback gestures, and persistence.

## Related documents

- [Presentation architecture](README.md)
- [Track-list presentation specification](track-presentation.md)
- [List presentation preference specification](list-preference.md)
- [Application managed-state reference](../../reference/persistence/application-config.md)
- [Persisted presentation state](../../reference/presentation/persisted-state.md)
