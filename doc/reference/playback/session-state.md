---
id: playback.session-state
---
# Playback session state

## Scope and version

This reference owns the exact serialized `PlaybackSessionState` payload in schema version `5`.
Capture, validation, restore, normalization, save scheduling, failure, discard, and shutdown behavior belongs to the [playback session persistence specification](../../system/playback/session-persistence.md).

## Code boundary

The payload belongs to the **application runtime** layer in the [system architecture](../../system/overview.md), under the [playback](../../system/playback/README.md) and [persistence](../../system/persistence/README.md) architectures.
`PlaybackSessionYamlSchema` explicitly maps and validates the payload before `PlaybackSessionPersistence` installs it through the runtime grouped configuration store.

## Surface

The literal top-level group is `playback-session`.
The schema requires every root field:

| Field | C++ type | Default | Meaning |
|---|---|---|---|
| `schemaVersion` | Unsigned 32-bit integer | `5` | Exact compatibility gate. |
| `libraryId` | Canonical UUID string | Nil UUID | Library the session was saved for. |
| `sourceListId` | `ListId` | Invalid id | Library-scoped source list. |
| `quickFilterExpression` | String | Empty | Ad-hoc filter applied over the source. |
| `sortBy` | Sequence of `TrackSortTerm` | Empty | Ordered field/direction terms. |
| `currentTrackId` | `TrackId` | Invalid id | Last restorable subject. |
| `currentTrackUri` | String | Empty | Library URI of that subject's file as the library last confirmed it, including after a relink while the app runs; the URI it was opened with when the library never confirmed another. A saved value is never empty. |
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

- `schemaVersion` must equal `5`.
- `libraryId` must be a canonical 8-4-4-4-12 hexadecimal UUID.
- `currentTrackUri` must be a canonical library URI.
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

Only version `5` is accepted.
There is no migration or seeded missing-field fallback for another/malformed version.
Version `5` includes the accepted predicate grammar, field binding, and Unicode-caseless `~` evaluation meaning of `quickFilterExpression`.
A predicate change that can emit quick-filter text an existing version-5 reader rejects, or can reject or reinterpret retained quick-filter text, requires a new playback schema version; the owner may then reject version `5` or provide an explicit tested migration.
The expression carries no separate language version.

Version `5` also includes the current numeric `TrackSortField` mapping.
Changing a sort-field raw value requires a playback schema version or an explicit version-5 compatibility implementation; the stable text ids used by presentation documents do not silently migrate this payload.

List and track ids are scoped to one library, so version 5 records that library's UUID and the current subject's URI.
The [persistence specification](../../system/playback/session-persistence.md#check-identity) drops a payload whose library or subject no longer matches.
Independently, the GTK library-switch lifecycle physically deletes the group and terminally seals persistence in the old process, fully destroys that graph, and activates a successor process with idle playback.
The successor admits future playback writes only after the matching root is durable; root-commit failure keeps the prior root, no payload, and a permanent write seal.

## Examples

```yaml
playback-session:
  schemaVersion: 5
  libraryId: "123e4567-e89b-12d3-a456-426614174000"
  sourceListId: 1
  quickFilterExpression: "$artist = 'Example'"
  sortBy: []
  currentTrackId: 42
  currentTrackUri: "Example/Album/01 Opening.flac"
  anchorIndex: 7
  positionMs: 12500
  shuffleMode: 0
  repeatMode: 0
  volume: 0.8
  muted: false
```

The runtime schema writes strong ids through their explicit unsigned raw values and writes playback and sort enums through the version-5 numeric mappings above.

## Implementation authority

- [`PlaybackSessionState.h`](../../../app/runtime/PlaybackSessionState.h) defines group, version, fields, defaults, and maximum sort terms.
- [`PlaybackSessionYamlSchema.h`](../../../app/runtime/PlaybackSessionYamlSchema.h) and [`PlaybackSessionYamlSchema.cpp`](../../../app/runtime/PlaybackSessionYamlSchema.cpp) define explicit field mapping, version dispatch, structural deserialization, and semantic validation.
- [`PlaybackSessionPersistence.cpp`](../../../app/runtime/PlaybackSessionPersistence.cpp) owns store invocation and restore installation.

## Test authority

- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp) protects every field, missing-field rejection, schema gate, semantic validation, round trip, and frozen stop/shutdown checkpoints.
- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) protects the explicit schema invocation and failed-candidate boundary used by this payload.

## Related documents

- [Playback session persistence](../../system/playback/session-persistence.md)
- [Playback architecture](../../system/playback/README.md)
- [Persistence and managed-state architecture](../../system/persistence/README.md)
- [Application managed-state surface](../persistence/application-config.md)
- [Predicate language](../query/predicate-language.md)
- [Runtime track-field catalog](../library/model/track-field.md)
- [Persisted presentation state](../presentation/persisted-state.md)
