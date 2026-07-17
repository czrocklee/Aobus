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
It defines the virtual root, effective parent relationships, malformed-parent recovery, sibling order, and the structure available to frontend adapters.

It does not own persisted list relationships, list mutation validation, GTK tree objects, terminal labels, selection, or workspace navigation.
Those facts belong to the [library list model](../../reference/library/model/list.md), [library mutation specification](../library/runtime/mutation.md), [presentation architecture](../../architecture/presentation.md), and frontend specifications.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places shared interactive projection in UIModel, and the [presentation architecture](../../architecture/presentation.md) defines its runtime and frontend dependencies.
`ListTreeProjection` under `app/include/ao/uimodel/library/list/` consumes frontend-neutral `rt::ListNode` values and contains no GTK or terminal types.
GTK and TUI consume the resulting roots, rows, and child edges without reconstructing list parentage or sibling order.

## Terminology

- The **source parent** is the `parentId` supplied by the runtime snapshot.
- The **effective parent** is the parent used by the presentation tree after validation and recovery.
- The **virtual root** is the permanent All Tracks row identified by `rt::kAllTracksListId`.
- A **parent cycle** is a closed chain of two or more list parent relationships.

## Invariants

- Every projection contains exactly one virtual All Tracks root, including for an empty snapshot.
- Every retained snapshot row preserves its list id, name, semantic kind, and local Smart List expression.
- A source parent becomes the effective parent only when it names another retained row and is not the row itself.
- A missing, invalid, or self parent is replaced by the virtual root.
- Each parent cycle is broken deterministically by assigning the lowest list id in that cycle to the virtual root; every other edge in the cycle remains intact.
- `ListTreeProjectionRow::parentId` is the effective parent and agrees with the corresponding parent row's `childIds` entry.
- Siblings are ordered by ascending list id independently of snapshot order and display name.
- A retained row occurs at most once in the effective tree.

## State model

`ListTreeProjection` is an owned value containing ordered root ids and a row map keyed by list id.
Each row contains its effective parent, semantic `ListNodeKind`, display name, optional local expression, and ordered child ids.

The projection is disposable presentation state.
Runtime list storage and mutation remain authoritative for the source snapshot.

## Commands and transitions

`buildListTreeProjection(snapshot)` performs one synchronous projection.
It installs the virtual root, retains one row per unique snapshot id, derives effective parents, breaks cycles, and then builds reciprocal child edges in stable id order.

Rebuilding from a later snapshot replaces the complete value.
The projection does not publish incremental changes or mutate the runtime snapshot.

## Failure and cancellation

Projection exposes no recoverable error or cancellation channel.
Malformed parent relationships use the deterministic recovery rules above instead of producing a partial tree or entering an unbounded traversal.

## Persistence and versioning

The projection is not persisted and has no independent compatibility version.
Persisted parent validation and format compatibility belong to the library model and transfer contracts.

## Frontend observations

GTK builds native tree nodes from the effective parent/child edges.
TUI walks the same roots and child edges in preorder, then adds terminal-specific indentation, kind icons, and expression detail.
Neither frontend independently sorts the snapshot or follows source-parent chains.

## Implementation map

- [`ListTreeProjection.h`](../../../app/include/ao/uimodel/library/list/ListTreeProjection.h) defines the shared value surface.
- [`ListTreeProjection.cpp`](../../../app/uimodel/library/list/ListTreeProjection.cpp) owns effective-parent recovery and stable child construction.
- GTK [`ListTreeModelBuilder.cpp`](../../../app/linux-gtk/list/ListTreeModelBuilder.cpp) adapts the projection to Gio/GTK tree objects.
- TUI [`LibraryNavigation.cpp`](../../../app/tui/LibraryNavigation.cpp) adapts it to terminal navigation rows.

## Test map

- [`ListTreeProjectionTest.cpp`](../../../test/unit/uimodel/library/list/ListTreeProjectionTest.cpp) protects nested rows, semantic kinds, invalid-parent recovery, deterministic cycle breaking, and sibling order.
- [`ListTreeModelBuilderTest.cpp`](../../../test/unit/linux-gtk/list/ListTreeModelBuilderTest.cpp) protects GTK tree adaptation.
- [`LibraryNavigationTest.cpp`](../../../test/unit/tui/LibraryNavigationTest.cpp) protects TUI preorder, indentation, icons, details, and malformed-parent adaptation.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Library architecture](../../architecture/library.md)
- [Library list model](../../reference/library/model/list.md)
- [Library mutation specification](../library/runtime/mutation.md)
- [TUI interaction specification](../tui/interaction.md)
