---
id: library.list-model
type: reference
status: current
domain: library
summary: Enumerates saved List identifiers, fields, hierarchy, local expressions, and optional order ranks.
---
# List model

## Scope and version

This reference defines the exact logical surface of a stored library list.
Mutation behavior belongs to [library access and mutation](../../../spec/library/runtime/mutation.md), and effective membership belongs to [track sources](../../../spec/library/source/track-source.md).

## Code boundary

The persisted list model belongs to the **core libraries** layer in the [system architecture](../../../architecture/system-overview.md), with builders/views under `include/ao/library/` and storage under `lib/library/`.
`ao::rt::ListNode` is an application-runtime value projection of that model, not a second persistence authority.

## Surface

| Field | Type | Meaning |
|---|---|---|
| `ListId` | Unsigned 32-bit | Durable list identity; zero is invalid. |
| `parentId` | `ListId` | Parent list; zero means the All Tracks root. |
| `name` | UTF-8 text | User-visible list name. |
| `description` | UTF-8 text | Optional description. |
| `filter` | UTF-8 text | Local predicate applied to the parent source; empty is the identity expression `true`. |
| `orderTrackIds` | Unique ordered `TrackId` sequence | Optional rank overlay; it never defines membership. |

The All Tracks root uses the reserved runtime identity `kAllTracksListId` and is not a normal stored list row.

## Unified semantics

Every stored row has the same model.
There is no persisted Manual, Smart, Folder, or Playlist kind, and `filter` and `orderTrackIds` are independent fields.

A List applies its local expression to its parent's effective ordered source.
Its optional ranks then place currently matching ranked ids first in stored order and append matching unranked ids in parent order.
An id in `orderTrackIds` that does not currently match is a hidden rank: it is absent from effective membership but resumes its stored position if it matches again.
A child consumes the parent's complete effective order, so hierarchy always denotes source derivation rather than display-only organization.

The exact filter text surface belongs to the [predicate language](../../../reference/query/predicate-language.md), and its runtime membership belongs to [track sources](../../../spec/library/source/track-source.md).

## Validation rules

Each text field is limited to 65,535 bytes by product policy even though the physical length field is 32-bit.
The stored order count must fit unsigned 32-bit, multiplication and aggregate record sizing use checked host-size arithmetic, and the record is padded canonically to a four-byte boundary.
`ListBuilder::serialize()` returns `ValueTooLarge` instead of narrowing an out-of-range count or size.

`ListBuilder::OrderTrackIdsBuilder` retains only the first occurrence of an id.
Runtime mutation and YAML import require resolved order ids to identify existing tracks; an order id need not currently match the List expression.
Parent relationships must identify an allowed root or existing list and must not create a cycle.
An empty expression is valid identity behavior; every non-empty expression must parse and compile before a committing create or update.

## Compatibility and versioning

The physical version is owned by the [library database reference](../storage/database.md).
The database version also gates how stored `filter` text parses, binds, and selects membership; the expression carries no separate language version.
Changing hierarchy meaning, expression membership, rank-overlay semantics, or predicate behavior for existing filter text is a behavioral and storage compatibility change.

## Implementation authority

- [`ListBuilder.h`](../../../../include/ao/library/ListBuilder.h) and [`ListView.h`](../../../../include/ao/library/ListView.h) define the logical core surface.
- [`ListLayout.h`](../../../../include/ao/library/ListLayout.h) defines the binary header.
- [`ListNode.h`](../../../../app/include/ao/rt/ListNode.h) defines the runtime read value.

## Test authority

- List builder, layout, store, and view tests under [`test/unit/library/`](../../../../test/unit/library/) lock the persisted surface.
- [`LibraryWriterListOrderTest.cpp`](../../../../test/unit/runtime/library/LibraryWriterListOrderTest.cpp) and [`LibraryWriterListMembershipTest.cpp`](../../../../test/unit/runtime/library/LibraryWriterListMembershipTest.cpp) lock rank and tag-backed editing semantics.
- [`ListOrderSourceTest.cpp`](../../../../test/unit/runtime/source/ListOrderSourceTest.cpp), [`ListOrderSourceObserverTest.cpp`](../../../../test/unit/runtime/source/ListOrderSourceObserverTest.cpp), and predicate-source tests lock effective membership and ordering.

## Related documents

- [Library database](../storage/database.md)
- [Library YAML format](../format/yaml.md)
- [Predicate language](../../../reference/query/predicate-language.md)
- [Track expression architecture](../../../architecture/track-expression.md)
