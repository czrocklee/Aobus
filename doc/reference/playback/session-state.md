---
id: playback.session-state
type: reference
status: current
domain: playback
summary: Enumerates the exact playback-session group, version 3 fields, types, defaults, transient exclusions, and compatibility gate.
---
# Playback session state

## Scope and version

This reference owns the exact serialized `PlaybackSessionState` payload in schema version `3`.
Capture, validation, restore, normalization, save scheduling, failure, discard, and shutdown behavior belongs to the [playback session persistence specification](../../spec/playback/session-persistence.md).

## Code boundary

The payload belongs to the **application runtime** layer in the [system architecture](../../architecture/system-overview.md), under the [playback](../../architecture/playback.md) and [persistence](../../architecture/persistence-and-managed-state.md) architectures.
`PlaybackSessionYamlSchema` explicitly maps and validates the payload before `PlaybackSessionPersistence` installs it through the runtime grouped configuration store.

## Surface

The literal top-level group is `playback-session`.
The schema requires every root field:

| Field | C++ type | Default | Meaning |
|---|---|---|---|
| `schemaVersion` | Unsigned 32-bit integer | `3` | Exact compatibility gate. |
| `sourceListId` | `ListId` | Invalid id | Library-scoped source list. |
| `quickFilterExpression` | String | Empty | Ad-hoc filter applied over the source. |
| `sortBy` | Sequence of `TrackSortTerm` | Empty | Ordered field/direction terms. |
| `currentTrackId` | `TrackId` | Invalid id | Last restorable subject. |
| `anchorIndex` | Unsigned 64-bit integer | `0` | Saved semantic cursor anchor. |
| `positionMs` | Unsigned 64-bit integer | `0` | Transport offset in milliseconds. |
| `shuffleMode` | `ShuffleMode` | `Off` | Current shuffle policy. |
| `repeatMode` | `RepeatMode` | `Off` | Current repeat policy. |
| `volume` | Float | `1.0` | Normalized application volume intent. |
| `muted` | Boolean | `false` | Mute intent. |

`sortBy` contains at most `kTrackSortFieldCount` terms.
Every term is an exact mapping:

| Field | Serialized value | Meaning |
|---|---|---|
| `field` | Signed 32-bit enum scalar. | Raw `TrackSortField` value from the [runtime track-field catalog](../library/model/track-field.md#sort-fields). |
| `ascending` | Boolean. | `true` for ascending and `false` for descending. |

This playback schema deliberately remains separate from the stable textual presentation-state vocabulary.

## Transient exclusions

The payload contains none of these:

- materialized track ids or a queue index;
- source lease, live projection rows, or source-validity state;
- workspace view id or selection;
- prepared-next ids or tokens;
- sticky shuffle candidate or shuffle history;
- decoder, Engine, audio, route, render, or callback generations;
- output backend/profile/device identity; or
- debounce task and scheduling generation.

## Validation rules

- `schemaVersion` must equal `3`.
- List and track identities must satisfy semantic restore validation against the active library.
- Anchor and position must convert to their runtime index/duration representations without overflow.
- Shuffle and repeat values must be supported enumerators.
- Volume must be finite and within `[0, 1]` for stored-state acceptance.
- Sort fields and directions must be valid, fields cannot repeat, and term count cannot exceed the field catalog.
- The quick filter must parse and compile under the active library vocabulary.
- Root and sort-term mappings reject unknown and duplicate fields; every sequence element is validated.
- Missing fields, wrong node kinds, malformed scalars, and malformed sort entries reject the complete candidate before live state changes.
- The schema checks `schemaVersion` first and returns `NotSupported` for another version before interpreting version-specific siblings.

## Compatibility and versioning

Only version `3` is accepted.
There is no migration or seeded missing-field fallback for another/malformed version.
Version `3` includes the accepted predicate grammar, field binding, and evaluation meaning of `quickFilterExpression`.
A predicate change that can emit quick-filter text an existing version-3 reader rejects, or can reject or reinterpret retained quick-filter text, requires a new playback schema version; the owner may then reject version `3` or provide an explicit tested migration.
The expression carries no separate language version.

Version `3` also includes the current numeric `TrackSortField` mapping.
Changing a sort-field raw value requires a playback schema version or an explicit version-3 compatibility implementation; the stable text ids used by presentation documents do not silently migrate this payload.

List and track ids are scoped to one library, but version 3 stores no library UUID.
The current GTK lifecycle prevents cross-library interpretation by discarding the group before replacement; RFC 0019 proposes a durable library binding.

## Examples

```yaml
playback-session:
  schemaVersion: 3
  sourceListId: 1
  quickFilterExpression: "artist = 'Example'"
  sortBy: []
  currentTrackId: 42
  anchorIndex: 7
  positionMs: 12500
  shuffleMode: 0
  repeatMode: 0
  volume: 0.8
  muted: false
```

The runtime schema writes strong ids through their explicit unsigned raw values and writes playback and sort enums through the version-3 numeric mappings above.

## Implementation authority

- [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h) defines group, version, fields, defaults, and maximum sort terms.
- [`PlaybackSessionYamlSchema.h`](../../../app/runtime/PlaybackSessionYamlSchema.h) and [`PlaybackSessionYamlSchema.cpp`](../../../app/runtime/PlaybackSessionYamlSchema.cpp) define explicit field mapping, version dispatch, structural deserialization, and semantic validation.
- [`PlaybackSessionPersistence.cpp`](../../../app/runtime/PlaybackSessionPersistence.cpp) owns store invocation and restore installation.

## Test authority

- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp) protects every field, missing-field rejection, schema gate, semantic validation, and round trip.
- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) protects the explicit schema invocation and failed-candidate boundary used by this payload.

## Related documents

- [Playback session persistence](../../spec/playback/session-persistence.md)
- [Playback architecture](../../architecture/playback.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Application managed-state surface](../persistence/application-config.md)
- [Predicate language](../query/predicate-language.md)
- [Runtime track-field catalog](../library/model/track-field.md)
- [Persisted presentation state](../presentation/persisted-state.md)
