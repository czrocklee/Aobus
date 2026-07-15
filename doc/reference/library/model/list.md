---
id: library.list-model
type: reference
status: current
domain: library
summary: Enumerates list identifiers, fields, kinds, hierarchy, and stored membership shape.
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
| `filter` | UTF-8 text | Local smart-list expression under the containing library format contract; non-empty selects smart kind. |
| `trackIds` | Ordered `TrackId` sequence | Stored manual membership; ignored by smart lists. |

The All Tracks root uses the reserved runtime identity `kAllTracksListId` and is not a normal stored list row.

## Kinds

- A smart list has non-empty `filter`; its stored `trackIds` have no membership effect.
- A manual list has empty `filter`; its explicit `trackIds` are stored intent.

A child smart list applies its local filter to its parent's effective membership.
A child manual list exposes the stable subsequence of stored ids currently present in its parent.

The exact filter text surface belongs to the [predicate language](../../../reference/query/predicate-language.md), and its runtime membership belongs to [track sources](../../../spec/library/source/track-source.md).

## Validation rules

Names, descriptions, filters, membership arrays, and aggregate serialized size must fit the `ListHeader` offset/length widths.
Parent relationships must identify an allowed root or existing list and must not create a cycle.
Smart expressions must compile before a committing create or update.
Full manual drafts contain unique existing track ids after canonicalization.

## Compatibility and versioning

The physical version is owned by the [library database reference](../storage/database.md).
The database version also gates how stored `filter` text parses, binds, and selects membership; the expression carries no separate language version.
Changing kind detection, hierarchy meaning, stored membership semantics, or predicate behavior for existing filter text is a behavioral and storage compatibility change.

## Implementation authority

- [`ListBuilder.h`](../../../../include/ao/library/ListBuilder.h) and [`ListView.h`](../../../../include/ao/library/ListView.h) define the logical core surface.
- [`ListLayout.h`](../../../../include/ao/library/ListLayout.h) defines the binary header.
- [`ListNode.h`](../../../../app/include/ao/rt/ListNode.h) defines the runtime read value.

## Test authority

- List builder, layout, store, and view tests under [`test/unit/library/`](../../../../test/unit/library/) lock the persisted surface.
- [`LibraryWriterManualListTest.cpp`](../../../../test/unit/runtime/library/LibraryWriterManualListTest.cpp) locks validation and membership command semantics.
- Manual and smart source tests under [`test/unit/runtime/source/`](../../../../test/unit/runtime/source/) lock effective membership.

## Related documents

- [Library database](../storage/database.md)
- [Library YAML format](../format/yaml.md)
- [Predicate language](../../../reference/query/predicate-language.md)
- [Track expression architecture](../../../architecture/track-expression.md)
