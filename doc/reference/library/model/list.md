---
id: library.list-model
---
# List model

## Scope and version

This reference defines the exact logical surface of a stored library list.
Mutation behavior belongs to [library access and mutation](../../../system/library/mutation.md), and effective membership belongs to [track sources](../../../system/library/track-source.md).

## Code boundary

The persisted list model belongs to the **core libraries** layer in the [system architecture](../../../system/overview.md), with builders/views under `include/ao/library/` and storage under `lib/library/`.
`ao::rt::ListNode` is an application-runtime value projection of that model, not a second persistence authority.

## Surface

| Field | Type | Meaning |
|---|---|---|
| `ListId` | Unsigned 32-bit | Durable list identity, never reused after deletion; zero is invalid and `UINT32_MAX` is never stored. |
| `parentId` | `ListId` | Parent list; zero means the All Tracks root. |
| `name` | Scalar-valid UTF-8 NFC text | User-visible list name. |
| `description` | Scalar-valid UTF-8 NFC text | Optional description. |
| `filter` | Syntactically opaque scalar-valid UTF-8 text | Byte-exact local predicate source interpreted by the application; empty is the identity expression `true`. |
| `orderTrackIds` | Unique ordered `TrackId` sequence | Optional rank overlay; it never defines membership, and it names only Track ids already allocated. |

The All Tracks root uses the reserved runtime identity `kAllTracksListId`, which is `UINT32_MAX`, and is not a normal stored list row.

## Unified semantics

Every stored row has the same model.
There is no persisted Manual, Smart, Folder, or Playlist kind, and `filter` and `orderTrackIds` are independent fields.

A List applies its local expression to its parent's effective ordered source.
Its optional ranks then place currently matching ranked ids first in stored order and append matching unranked ids in parent order.
An id in `orderTrackIds` that does not currently match is a hidden rank: it is absent from effective membership but resumes its stored position if it matches again.
A child consumes the parent's complete effective order, so hierarchy always denotes source derivation rather than display-only organization.

The exact filter text surface belongs to the [predicate language](../../query/predicate-language.md), and its runtime membership belongs to [track sources](../../../system/library/track-source.md).

### Tag-backed Lists

A List whose local expression is exactly one positive tag is what the frontends call a Playlist.
Add and Remove to Playlist edit its membership by adding and removing that tag on Tracks, as [tag-backed membership editing](../../../system/library/mutation.md#tag-backed-membership-editing) specifies, and its membership is still computed from the tag.
[Decision 0001](../../../decision/0001-unified-list-ordering.md) gives membership that single authority, with these consequences:

- Membership is a set. A Track is a member at most once, adding a Track that already carries the tag changes nothing, and `orderTrackIds` holds each id at most once, so a Playlist cannot repeat a Track.
- Playlists of the same tag share membership: adding a Track to one adds it to every other whose parent source contains it, and the tag also changes the membership of any other List whose expression uses it.
- Tags are Track data. A `list-only` [YAML transfer](../../../system/library/yaml-transfer.md) carries a List's filter and ranks but no tags, so in the target its members are the Tracks that already carry the tag there, and the ranks of its other Tracks stay hidden.
- No command renames a tag. Removing the tag from every Track empties the List, and its filter still names the tag.

## Validation rules

Each text field is validated as scalar UTF-8 and limited to 65,535 bytes by product policy even though the physical length field is 32-bit; name and description are normalized to NFC, while filter source retains its original bytes.
The stored order count must fit unsigned 32-bit, multiplication and aggregate record sizing use checked host-size arithmetic, and the record is padded canonically to a four-byte boundary.
`ListBuilder` owns its text fields rather than retaining setter input views.
`ListBuilder::prepare()` rejects malformed UTF-8, returns a recoverable error instead of narrowing an out-of-range count or text size, snapshots the admitted encoded bytes, and accepts no zero saved-order id.
The same canonical row validator checks preparation and open-time row validation, including NFC display text and scalar-valid byte-exact filter source.
The Store encoder's `AO_ENSURES` postcondition checks that its output exactly equals the validated immutable snapshot instead of allocating a second saved-order uniqueness set.

`ListBuilder::OrderTrackIdsBuilder` retains only the first occurrence of an id.
Runtime mutation and YAML import require resolved order ids to identify existing tracks; an order id need not currently match the List expression.
Parent relationships must identify an allowed root or existing list and must not create a cycle.
Those reference and topology rules belong above the local row validator.
The core List builder and Store preserve every filter string byte-exactly within the byte-size limit without parsing or compiling it.
Application authoring may reject an invalid expression for immediate feedback, while materializing invalid stored text exposes the empty-membership expression-error state defined by [track sources](../../../system/library/track-source.md).

## Compatibility and versioning

The physical version is owned by the [library database reference](../storage/database.md).
The database version gates the List record layout, rank representation, and text invariant.
Stored `filter` bytes carry no nested language version, so the database version also names their predicate language: a grammar or semantic change that alters the outcome of filter text the current language accepts increments it, as the [database versioning rules](../storage/database.md#compatibility-and-versioning) state.
Database admission never parses a filter; text that does not parse or compile is an application expression outcome rather than database corruption.
Changing hierarchy or rank-overlay persistence semantics remains a storage compatibility change.

## Implementation authority

- [`ListBuilder.h`](../../../../include/ao/library/ListBuilder.h) and [`ListView.h`](../../../../include/ao/library/ListView.h) define the logical core surface.
- [`ListLayout.h`](../../../../include/ao/library/ListLayout.h) defines the binary header.
- [`ListNode.h`](../../../../app/include/ao/rt/ListNode.h) defines the runtime read value.

## Test authority

- List builder, layout, store, and view tests under [`test/unit/library/`](../../../../test/unit/library) lock the persisted surface.
- [`LibraryCommandsListOrderTest.cpp`](../../../../test/unit/runtime/library/LibraryCommandsListOrderTest.cpp) and [`LibraryCommandsListMembershipTest.cpp`](../../../../test/unit/runtime/library/LibraryCommandsListMembershipTest.cpp) lock rank and tag-backed editing semantics.
- [`ListOrderSourceTest.cpp`](../../../../test/unit/runtime/source/ListOrderSourceTest.cpp), [`ListOrderSourceObserverTest.cpp`](../../../../test/unit/runtime/source/ListOrderSourceObserverTest.cpp), and predicate-source tests lock effective membership and ordering.

## Related documents

- [Library database](../storage/database.md)
- [Library YAML format](../format/yaml.md)
- [Predicate language](../../query/predicate-language.md)
- [Track expression architecture](../../../system/query/README.md)
