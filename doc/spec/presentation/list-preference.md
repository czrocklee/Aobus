---
id: presentation.list-preference
type: spec
status: current
domain: presentation
summary: Defines per-list presentation selection, recommendation fallback, persistence ownership, and filter independence.
---
# List presentation preference

## Scope

This specification defines how Aobus chooses and remembers the presentation normally used for one library list.
The presentation's grouping, sorting, and field behavior belongs to [track-list presentation](track-presentation.md), and exact built-in ids belong to the [track preset reference](../../reference/presentation/track-preset.md).

List content and Smart List expressions belong to the [list model](../../reference/library/model/list.md).
Transient quick-filter behavior belongs to [track filtering](track-filter.md).

## Code boundary

This contract spans the **application runtime**, **UIModel**, and GTK persistence adapter layers from the [system architecture](../../architecture/system-overview.md), as refined by the [presentation](../../architecture/presentation.md), [workspace](../../architecture/workspace.md), and [persistence and managed-state](../../architecture/persistence-and-managed-state.md) architectures.
Runtime owns the active `TrackPresentationSpec`; UIModel owns the preference map and recommendation policy; GTK owns the per-library persistence location and flush boundary.

## Terminology

- **List content** is the membership intent or Smart List predicate owned by the library.
- **Presentation preference** is an optional `ListId -> presentation id` choice.
- **Active presentation** is the exact `TrackPresentationSpec` installed in one runtime view.
- **Recommendation** is the source-aware fallback used when no resolvable preference exists.
- **Replay** restores a recorded view snapshot without treating that restoration as a new user preference.

## Invariants

- Library list records never contain presentation ids, column layouts, or view preferences.
- A presentation preference never changes list membership.
- A transient filter narrows the current list while retaining its current presentation.
- Preferences key by base `ListId`, not by `(ListId, filterExpression)`.
- Unknown saved presentation ids fall back instead of blocking list opening.
- Runtime navigation snapshots retain exact presentation state independently of the saved default preference.
- Replaying history does not write a new preference.
- GTK presentation preferences are per library and do not become global application state.

## State model

`ListPresentationPreferenceStore` retains a map from valid `ListId` to non-empty presentation id.
It also borrows the presentation catalog used to resolve built-in and custom ids.

Setting an empty id clears that list's preference.
Setting a preference for the invalid list id has no effect.
Bulk state replacement emits one general change only when the map actually differs.

The active runtime view separately retains:

- base `ListId`;
- transient `filterExpression`;
- exact `TrackPresentationSpec`;
- selection and revision state.

The preference map is a default-selection authority, not a mirror of every active or historical view.

## Resolution

Presentation resolution for a list follows this order:

1. Resolve a saved presentation id through the current built-in/custom catalog.
2. If no saved id exists or the id is unavailable, use source-aware recommendation.
3. Recommendation resolves its selected id through the same available catalog and falls back to the first built-in spec if needed.

Current source-aware recommendation is:

- manual list: `list-order`;
- Smart List: inspect its successfully parsed local filter and choose by the priority in [track-list presentation](track-presentation.md#recommendation);
- All Tracks or another non-Smart source: `albums`.

An invalid Smart List expression falls back to `albums` recommendation and does not change source membership or error state.

## Commands and transitions

Opening a list resolves its preferred/recommended presentation and supplies that spec when creating or updating the runtime view.
Changing the presentation through a normal user-selection path installs the new runtime spec and may update the base list's saved preference.

Applying a quick filter changes only `filterExpression` and active source/projection resources.
It does not create a new preference or rerun list recommendation.
If the list was displayed as albums before filtering, it remains displayed as albums unless a separate presentation command changes it.

Navigation history stores the exact presentation snapshot alongside base list and filter text.
Back/forward restoration applies that snapshot as replay and must not reinterpret it as a preference edit.

## Failure and cancellation

An unknown or removed presentation id is recoverable and selects the recommendation path.
An empty built-in catalog may produce an empty fallback spec; ordinary application composition supplies the built-in catalog.

UIModel preference operations and recommendation are synchronous and have no cancellation point.
GTK load/flush failures follow the frontend-owned preference fallback policy and do not mutate library list records.

## Persistence and versioning

GTK persists the preference map with other per-library track-view layout state through `GtkLayoutStateStore` in the library-specific `gtk_layout.yaml` store.
The exact configuration group and payload association belong to the [application managed-state surface](../../reference/persistence/application-config.md#group-registry).

The persisted value is a presentation id, so changing or removing a built-in id requires a compatibility path.
Unknown custom ids remain tolerated because custom presentations may be removed independently.

TUI currently uses runtime presentation state but does not use the GTK per-library preference store.

## Frontend observations

Presentation pickers resolve labels and specs through the shared catalog.
The active view observes the selected `TrackPresentationSpec`; frontends do not read or write list storage to remember the choice.

Quick-filter controls and Smart List editors may display the current presentation, but filter editing remains independent from preference mutation.

## Implementation map

- [`ListPresentationPreferenceStore`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceStore.h) owns the map and resolution order.
- [`TrackPresentationRecommender`](../../../app/include/ao/uimodel/library/presentation/TrackPresentationRecommender.h) owns source-aware fallback policy.
- [`TrackPresentationCatalog`](../../../app/include/ao/uimodel/library/presentation/TrackPresentationCatalog.h) resolves built-in and custom ids.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) owns active presentation state.
- [`WorkspaceService`](../../../app/include/ao/rt/WorkspaceService.h) owns view navigation snapshots and replay under the [workspace navigation specification](../workspace/navigation.md).
- [`GtkLayoutStateStore`](../../../app/linux-gtk/app/GtkLayoutStateStore.h) owns GTK per-library serialization.

## Test map

- [`ListPresentationPreferenceStoreTest.cpp`](../../../test/unit/uimodel/library/presentation/ListPresentationPreferenceStoreTest.cpp) proves map behavior, resolution, and fallbacks.
- [`TrackPresentationRecommenderTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackPresentationRecommenderTest.cpp) proves source-aware recommendations.
- [`GtkLayoutStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp) proves per-library persistence.
- Workspace history tests under [`test/unit/runtime/`](../../../test/unit/runtime/) prove snapshot replay semantics.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Track-list presentation](track-presentation.md)
- [Track filtering](track-filter.md)
- [Track presentation presets](../../reference/presentation/track-preset.md)
- [Workspace navigation](../workspace/navigation.md)
