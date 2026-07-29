---
id: presentation.list-tree
type: spec
status: current
domain: presentation
summary: Defines the shared list-navigation tree, effective-parent recovery, ordering, and frontend adaptation contract.
---
# List-navigation tree specification

## Scope

This specification owns the platform-neutral projection of a runtime list snapshot into the hierarchy consumed by interactive frontends.
It defines the independent system root, saved-root forest, effective parent relationships, malformed-parent recovery, sibling order, and the structure available to frontend adapters.

It does not own persisted list relationships, list mutation validation, GTK tree objects, terminal labels, selection, or workspace navigation.
Those facts belong to the [library list model](../../reference/library/model/list.md), [library mutation specification](../library/runtime/mutation.md), [presentation architecture](../../architecture/presentation.md), and frontend specifications.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places shared interactive projection in UIModel, and the [presentation architecture](../../architecture/presentation.md) defines its runtime and frontend dependencies.
`ListTreeProjection` under `app/include/ao/uimodel/library/list/` consumes frontend-neutral `rt::ListNode` values and contains no GTK or terminal types.
GTK and TUI consume the resulting roots, rows, and child edges without reconstructing list parentage or sibling order.

## Terminology

- The **source parent** is the `parentId` supplied by the runtime snapshot.
- The **effective parent** is the parent used by the presentation tree after validation and recovery.
- The **system root** is the permanent All Tracks row identified by `rt::kAllTracksListId`.
- A **saved root** is a saved List whose effective navigation parent is invalid.
  Its source still derives from All Tracks, but it is a sibling of the system root in navigation rather than a child row beneath it.
- A **parent cycle** is a closed chain of two or more list parent relationships.

## Invariants

- Every projection contains exactly one independent All Tracks system root, including for an empty snapshot.
- All Tracks has invalid navigation parent and no saved-List children.
- Root order is All Tracks first, followed by saved roots in ascending List id; frontends may render a physical section separator between those two root classes.
- Every retained snapshot row preserves its list id, name, and local filter expression.
- A source parent becomes the effective parent only when it names another retained row and is not the row itself.
- A missing, invalid, virtual, or self parent becomes an invalid effective navigation parent, making that row a saved root.
- Each parent cycle is broken deterministically by making the lowest list id in that cycle a saved root; every other edge in the cycle remains intact.
- For every non-root saved row, `ListTreeProjectionRow::parentId` agrees with the corresponding saved parent row's `childIds` entry.
- Saved roots occur in `rootIds` and in no row's `childIds`; the All Tracks system root obeys the same root reciprocity.
- Siblings are ordered by ascending list id independently of snapshot order and display name.
- A retained row occurs at most once in the effective tree.

## State model

`ListTreeProjection` is an owned value containing ordered root ids and a row map keyed by list id.
Each row contains its effective parent, display name, system-row marker, local expression, and ordered child ids.
Saved Lists have one shared row shape; hierarchy is expressed only by `parentId`, not by a persisted Folder or List kind.

The projection is disposable presentation state.
Runtime list storage and mutation remain authoritative for the source snapshot.

## Commands and transitions

`buildListTreeProjection(snapshot)` performs one synchronous projection.
It installs the independent All Tracks root, retains one row per unique snapshot id, derives effective parents, breaks cycles, records saved roots beside All Tracks, and then builds reciprocal saved-List child edges in stable id order.

Rebuilding from a later snapshot replaces the complete value.
The projection does not publish incremental changes or mutate the runtime snapshot.

## Failure and cancellation

Projection exposes no recoverable error or cancellation channel.
Malformed parent relationships use the deterministic recovery rules above instead of producing a partial tree or entering an unbounded traversal.

## Persistence and versioning

The projection is not persisted and has no independent compatibility version.
Persisted parent validation and format compatibility belong to the library model and transfer contracts.

## Frontend observations

GTK builds native tree nodes from the effective parent/child edges and uses root sections to render All Tracks separately from the saved-List forest.
TUI walks the same roots and child edges in preorder, then adds terminal-specific indentation, one shared List icon, and expression detail.
Neither frontend independently sorts the snapshot or follows source-parent chains.

## Implementation map

- [`ListTreeProjection.h`](../../../app/include/ao/uimodel/library/list/ListTreeProjection.h) defines the shared value surface.
- [`ListTreeProjection.cpp`](../../../app/uimodel/library/list/ListTreeProjection.cpp) owns effective-parent recovery and stable child construction.
- GTK [`ListTreeModelBuilder.cpp`](../../../app/linux-gtk/list/ListTreeModelBuilder.cpp) adapts the projection to Gio/GTK tree objects.
- TUI [`LibraryNavigation.cpp`](../../../app/tui/LibraryNavigation.cpp) adapts it to terminal navigation rows.

## Test map

- [`ListTreeProjectionTest.cpp`](../../../test/unit/uimodel/library/list/ListTreeProjectionTest.cpp) protects nested rows, unified row data, invalid-parent recovery, deterministic cycle breaking, and sibling order.
- [`ListTreeModelBuilderTest.cpp`](../../../test/unit/linux-gtk/list/ListTreeModelBuilderTest.cpp) protects GTK tree adaptation.
- [`LibraryNavigationTest.cpp`](../../../test/unit/tui/LibraryNavigationTest.cpp) protects TUI preorder, indentation, icons, details, and malformed-parent adaptation.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Library architecture](../../architecture/library.md)
- [Library list model](../../reference/library/model/list.md)
- [Library mutation specification](../library/runtime/mutation.md)
- [TUI interaction specification](../tui/interaction.md)
