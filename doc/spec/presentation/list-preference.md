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

List membership expressions and saved rank overlays belong to the [list model](../../reference/library/model/list.md).
Transient quick-filter behavior belongs to [track filtering](track-filter.md).

## Code boundary

This contract spans the **application runtime**, **UIModel**, and frontend persistence adapters from the [system architecture](../../architecture/system-overview.md), as refined by the [presentation](../../architecture/presentation.md), [workspace](../../architecture/workspace.md), and [persistence and managed-state](../../architecture/persistence-and-managed-state.md) architectures.
Runtime owns the active `TrackPresentationSpec`; UIModel owns the preference map, deletion lifecycle, recommendation policy, and versioned semantic schema; each frontend owns its per-library persistence location and save boundary.

## Terminology

- **List membership** is the local predicate composed with parent membership by the library.
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
- Presentation preferences are per library and do not become global application state.
- Deleting a saved List removes its preference in the shared UIModel lifecycle; one cascade change removes every deleted descendant preference.

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

- saved List with a nonempty, successfully parsed local expression: choose by the priority in [track-list presentation](track-presentation.md#recommendation);
- saved List with an empty expression: `albums`;
- All Tracks: `albums`.

An invalid saved-List expression falls back to `albums` recommendation and does not change source membership or error state.
`Manual Order` is an explicit user preference rather than a recommendation inferred from List storage.
The New Playlist template selects that preference when it creates a tag-backed List.

## Commands and transitions

When GTK opens a plain list target, it always resolves the preferred or recommended presentation and submits it to `WorkspaceService` as `NewViewDefault` intent.
`WorkspaceService` alone decides whether an existing unfiltered view is reusable.
Reuse ignores that default and retains the view's exact active presentation; creation applies the resolved default to the new plain view.
A filtered view over the same list is not a reusable plain target, so `WorkspaceService` creates the plain view with the resolved default and does not alter the filtered view.

Workspace restoration installs the exact presentation stored with each view and does not apply the preference map afterward.
Navigation-history replay likewise restores the recorded exact presentation without resolving a new default.
Playback restoration submits the same `NewViewDefault` request as ordinary GTK navigation: an existing plain view keeps its exact presentation, while a newly created plain view receives the preference or recommendation.
Changing the presentation through a normal user-selection path installs the new runtime spec and may update the base list's saved preference.
When a committed `LibraryChangeSet` deletes Lists, the shared preference lifecycle erases every corresponding key before a frontend persists its next state.

WinUI retains one `ListPresentationPreferenceStore` for the active library session and resolves every saved id through `presentationForList()` before applying it to a newly bound or navigated view.
An unavailable id therefore applies the source-aware recommendation without deleting or rewriting the opaque saved id.
If applying the resolved `TrackPresentationSpec` returns an Error, WinUI presents the Error and retains both the current active presentation and the saved preference.
Its presentation menu uses the shared picker model and catalog, including restored custom presets, and checkpoints a changed preference only after the runtime accepts the complete specification.

Applying a quick filter changes only `filterExpression` and active source/projection resources.
It does not create a new preference or rerun list recommendation.
If the list was displayed as albums before filtering, it remains displayed as albums unless a separate presentation command changes it.

Navigation history stores the exact presentation snapshot alongside base list and filter text.
Back/forward restoration applies that snapshot as replay and must not reinterpret it as a preference edit.

## Failure and cancellation

An unknown or removed presentation id is recoverable and selects the recommendation path.
An empty built-in catalog may produce an empty fallback spec; ordinary application composition supplies the built-in catalog.

UIModel preference operations, recommendation, deletion cleanup, and persistence conversion are synchronous and have no cancellation point.
An unsupported version, duplicate or invalid list id, empty id, or structural mismatch rejects the complete persisted preference group and preserves the caller's seeded state.
An unknown nonempty presentation id remains a valid extensible reference and follows recommendation fallback.
GTK load/save failures do not mutate library list records.
Bulk installation during GTK restore suppresses persistence callbacks, so loading one valid sibling group cannot rewrite another rejected group.

## Persistence and versioning

GTK persists the preference map with other per-library track-view layout state through `GtkLayoutStateStore` in the library-specific `gtk_layout.yaml` store.
WinUI persists the same semantic group in its platform application settings, keeps opaque ids across window/session replacement, and constructs the shared committed-List deletion lifecycle for each new active session.
The `trackView.presentations` group carries required `version: 1` and represents the map as a sequence of `{listId, presentationId}` entries so duplicate identities can be rejected before map construction.
The exact fields belong to the [persisted presentation-state reference](../../reference/presentation/persisted-state.md); group registration belongs to the [application managed-state surface](../../reference/persistence/application-config.md#group-registry).

The persisted value is a presentation id, so changing or removing a built-in id requires a compatibility path.
Unknown custom ids remain tolerated because custom presentations may be removed independently; fallback resolution never rewrites that opaque value.
Unversioned legacy preference maps and unsupported future versions are rejected without migration or automatic rewrite.
The explicit schema returns `NotSupported` for a future version before interpreting its preferences.

TUI currently uses runtime presentation state but does not persist this preference map.

## Frontend observations

Presentation pickers resolve labels and specs through the shared catalog.
The active view observes the selected `TrackPresentationSpec`; frontends do not read or write list storage to remember the choice.

Quick-filter controls and List editors may display the current presentation, but filter editing remains independent from preference mutation.

## Implementation map

- [`ListPresentationPreferenceStore`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceStore.h) owns the map and resolution order.
- [`ListPresentationPreferenceLifecycle`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceLifecycle.h) owns shared cleanup from `listsDeleted`.
- [`ListPresentationPreferenceYamlSchema`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h) owns explicit YAML mapping, the versioned document, and semantic conversion.
- [`TrackPresentationRecommender`](../../../app/include/ao/uimodel/library/presentation/TrackPresentationRecommender.h) owns source-aware fallback policy.
- [`TrackPresentationCatalog`](../../../app/include/ao/uimodel/library/presentation/TrackPresentationCatalog.h) resolves built-in and custom ids.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) owns active presentation state.
- [`WorkspaceService`](../../../app/include/ao/rt/WorkspaceService.h) owns view navigation snapshots and replay under the [workspace navigation specification](../workspace/navigation.md).
- [`GtkLayoutStateStore`](../../../app/linux-gtk/app/GtkLayoutStateStore.h) owns GTK per-library serialization; WinUI [`LibrarySession`](../../../app/windows-winui/app/LibrarySession.h) owns its retained preference store and platform settings checkpoint, while [`PresentationButtonComponent`](../../../app/windows-winui/layout/component/track/TrackRegistry.cpp) adapts the shared picker model.

## Test map

- [`ListPresentationPreferenceStoreTest.cpp`](../../../test/unit/uimodel/library/presentation/ListPresentationPreferenceStoreTest.cpp) proves map behavior, resolution, fallbacks, and cascade cleanup.
- [`ListPresentationPreferenceYamlSchemaTest.cpp`](../../../test/unit/uimodel/library/presentation/ListPresentationPreferenceYamlSchemaTest.cpp) proves version gates, opaque ids, and whole-group rejection.
- [`TrackPresentationRecommenderTest.cpp`](../../../test/unit/uimodel/library/presentation/TrackPresentationRecommenderTest.cpp) proves source-aware recommendations.
- [`GtkLayoutStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp) proves per-library persistence.
- [`MainWindowSessionPresentationTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowSessionPresentationTest.cpp) proves GTK creation, reuse, workspace restoration, history replay, and playback-restoration precedence.
- Workspace history tests under [`test/unit/runtime/`](../../../test/unit/runtime/) prove snapshot replay semantics.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Track-list presentation](track-presentation.md)
- [Track filtering](track-filter.md)
- [Track presentation presets](../../reference/presentation/track-preset.md)
- [Persisted presentation state](../../reference/presentation/persisted-state.md)
- [Workspace navigation](../workspace/navigation.md)
