---
id: presentation.track-preset
type: reference
status: current
domain: presentation
summary: Enumerates built-in track-presentation preset identities, menu order, grouping intent, and defaults.
---
# Track presentation presets

## Scope and version

This reference enumerates the built-in `TrackPresentationPreset` catalog.
Preset ids are persistence-facing identifiers.

## Code boundary

This exact catalog belongs to the **application runtime** layer in the [system architecture](../../architecture/system-overview.md) and is consumed through the [presentation architecture](../../architecture/presentation.md).
Its public values live in `app/include/ao/rt/TrackPresentation.h` and its authoritative definitions live in `app/runtime/TrackPresentation.cpp`; UIModel and frontends adapt the catalog but do not redefine its ids or shapes.

## Surface

| Order | Id | Label | Grouping and intent |
|---:|---|---|---|
| 1 | `library` | Library | Flat album-artist/album/disc/track order; global default. |
| 2 | `list-order` | List Order | Flat source order; manual-list recommendation. |
| 3 | `songs` | Songs | Flat title/artist/album order. |
| 4 | `albums` | Albums | Album groups with track-oriented columns. |
| 5 | `artists` | Artists | Album-artist groups with discography order. |
| 6 | `performers` | Performers | Track-artist groups. |
| 7 | `genres` | Genres | Genre groups with album-artist discography order. |
| 8 | `years` | Years | Year groups with album-artist discography order. |
| 9 | `classical-composers` | Classical: Composers | Composer groups with Work and Movement columns. |
| 10 | `classical-conductors` | Classical: Conductors | Conductor groups with Work, Composer, Ensemble, and Movement columns. |
| 11 | `classical-works` | Classical: Works | Work groups with Movement rows. |
| 12 | `tagging` | Tagging | Flat curation fields with raw disc/track numbers and tags. |
| 13 | `technical` | Technical | Flat technical summary, file size, and path. |

The catalog order is the frontend menu order.
Custom presets carry their own stable id, label, base preset id, and normalized presentation spec and follow the built-ins in catalogs.
Exact field ids and sort/group enum values used by those specs belong to the [runtime track field catalog](../library/model/track-field.md).

## Validation rules

`normalizeTrackPresentationSpec` removes duplicate field entries and replaces an empty id with the default `library` id.
Persisted selection must resolve by id; unknown ids fall back through the owning preference contract.

## Compatibility and versioning

Changing a built-in id breaks persisted selection and requires an explicit compatibility path.
Labels, descriptions, fields, grouping, and sorting may evolve only with matching presentation tests and behavioral documentation.

## Implementation authority

- [`TrackPresentation.cpp`](../../../app/runtime/TrackPresentation.cpp) owns the exact catalog.
- [`TrackPresentation.h`](../../../app/include/ao/rt/TrackPresentation.h) owns preset and spec shapes.

## Test authority

- [`TrackPresentationTest.cpp`](../../../test/unit/runtime/TrackPresentationTest.cpp) locks the established catalog ids and most preset shapes; `classical-conductors` still needs its own focused shape assertion.
- UIModel and frontend presentation tests lock selection and rendering adaptation.

## Related documents

- [Track-list presentation](../../spec/presentation/track-presentation.md)
- [List presentation preference](../../spec/presentation/list-preference.md)
- [Track model](../library/model/track.md)
- [Runtime track field catalog](../library/model/track-field.md)
