---
id: presentation.track-preset
---
# Track presentation presets

## Scope and version

This reference enumerates the runtime built-in `TrackPresentationPreset` identities, menu order, and structural intent.
Preset ids are persistence-facing identifiers; the exact field, grouping, and sort vectors remain source-owned by `TrackPresentation.cpp` rather than duplicated here.
Exact built-in labels and descriptions belong to the [presentation text catalog](../../system/presentation/text-catalog.md).

## Code boundary

The stable ids and structural specs belong to the **application runtime** layer in the [system architecture](../../system/overview.md) and are consumed through the [presentation architecture](../../system/presentation/README.md).
Their public values live in `app/include/ao/rt/TrackPresentation.h` and their authoritative definitions live in `app/runtime/TrackPresentation.cpp`.
UIModel joins those values with `builtinTrackPresentation` over `MessageCatalog`; frontends do not redefine ids, shapes, or shared authored copy.

## Surface

| Order | Id | Grouping and structural intent |
|---:|---|---|
| 1 | `library` | Flat album-artist/album/disc/track order; global default. |
| 2 | `list-order` | Flat source order; user-facing **Manual Order**, selected explicitly or by the New Playlist template. |
| 3 | `songs` | Flat title/artist/album order. |
| 4 | `albums` | Album groups with track-oriented columns. |
| 5 | `artists` | Album-artist groups with discography order. |
| 6 | `performers` | Track-artist groups. |
| 7 | `genres` | Genre groups with album-artist discography order. |
| 8 | `years` | Year groups with album-artist discography order. |
| 9 | `classical-composers` | Composer groups with Work and Movement columns. |
| 10 | `classical-conductors` | Conductor groups with Work, Composer, Ensemble, and Movement columns. |
| 11 | `classical-works` | Work groups with Movement rows. |
| 12 | `tagging` | Flat curation fields with raw disc/track numbers and tags. |
| 13 | `technical` | Flat technical summary, file size, and path. |

The catalog order is the frontend menu order.
Custom presets carry their own stable id, user-authored label, base preset id, and normalized presentation spec and follow the built-ins in catalogs.
Exact field ids and sort/group enum values used by those specs belong to the [runtime track field catalog](../library/model/track-field.md).
`list-order` has no special storage semantics: its empty `sortBy` exposes source order, and saved Lists may write their independent rank overlay only while the active presentation is flat and unsorted.
All Tracks does not own such an overlay, so frontends disable `list-order` for that virtual source.

## Validation rules

`normalizeTrackPresentationSpec` removes duplicate visible and redundant field entries, replaces an empty id with the default `library` id, and supplies `Title` when the visible-field vector is empty.
Persisted selection must resolve by id; unknown ids fall back through the owning preference contract.

## Compatibility and versioning

Changing a built-in id breaks persisted selection and requires an explicit compatibility path.
Fields, grouping, and sorting may evolve only with matching runtime presentation tests and behavioral documentation.
Labels and descriptions may evolve only with matching UIModel catalog tests and catalog-reference updates.

## Implementation authority

- [`TrackPresentation.cpp`](../../../app/runtime/TrackPresentation.cpp) owns the exact catalog.
- [`TrackPresentation.h`](../../../app/include/ao/rt/TrackPresentation.h) owns preset and spec shapes.
- [`TrackPresentationText.cpp`](../../../app/uimodel/library/presentation/TrackPresentationText.cpp) owns built-in labels and descriptions.

## Test authority

- [`TrackPresentationTest.cpp`](../../../test/unit/runtime/TrackPresentationTest.cpp) locks most established ids and preset shapes; `classical-conductors` currently has neither an id-presence check nor a focused shape assertion.
- [`PresentationTextFeaturesTest.cpp`](../../../test/unit/uimodel/presentation/PresentationTextFeaturesTest.cpp) locks text coverage; UIModel and frontend presentation tests lock selection and rendering adaptation.

## Related documents

- [Track-list presentation](../../system/presentation/track-presentation.md)
- [List presentation preference](../../system/presentation/list-preference.md)
- [Track model](../library/model/track.md)
- [Runtime track field catalog](../library/model/track-field.md)
- [Presentation text catalog](../../system/presentation/text-catalog.md)
