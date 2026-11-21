---
id: presentation.list-preference
---

# List presentation preference

## Scope

This specification defines how Aobus chooses and remembers the presentation normally used for one library list.
The presentation's grouping, sorting, and field behavior belongs to [track-list presentation](track-presentation.md), and exact built-in ids belong to the [track preset reference](../../reference/presentation/track-preset.md).

List membership expressions and saved rank overlays belong to the [list model](../../reference/library/model/list.md).
Transient quick-filter behavior belongs to [track filtering](track-filter.md).

## Code boundary

This contract spans the **application runtime**, **UIModel**, and frontend persistence adapters from the [system architecture](../overview.md), as refined by the [presentation](README.md), [workspace](../workspace/README.md), and [persistence and managed-state](../persistence/README.md) architectures.
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

`ListPresentations` retains a map from valid `ListId` to non-empty presentation id.
It also borrows the presentation catalog used to resolve built-in and custom ids.

`presentationIdForList()` returns a borrowed view into that map. The view remains valid only while the owner and that entry remain unchanged; replacement, clearing, effective restore, library reset, or deletion of the same List invalidates it. Callers copy the id before a reentrant callback, coroutine suspension, or any such mutation.

Setting an empty id clears that list's preference.
An empty id is Auto: absence of a saved preference, not today's concrete recommendation. The recommendation is resolved when a view opens, so no migration reinterprets an already stored concrete builtin id.
Setting a preference for the invalid list id has no effect.
Bulk state replacement emits one general change only when the map actually differs.

The active runtime view separately retains:

- base `ListId`;
- transient `filterExpression`;
- exact `TrackPresentationSpec`;
- selection and revision state.

The preference map is a default-selection authority, not a mirror of every active or historical view.

## Resolution

Resolve an eligible saved id through the current built-in and custom preset catalog. If the id is absent, unavailable, or ineligible, use the source-aware recommendation without changing the stored id. Recommendation resolves its result through the same catalog and falls back to the first built-in spec if needed.

The eligibility rule excludes only built-in `list-order` (Manual Order) on All Tracks, not a custom flat unsorted spec. A picker or preset command checks eligibility before changing the view, history, or preference. The map can still contain an ineligible id, and a captured view can still contain the exact spec: neither reading a default nor replaying a view is a picker action.

Current source-aware recommendation is:

- saved List with a nonempty, successfully parsed local expression: choose by the priority in [track-list presentation](track-presentation.md#recommendation);
- saved List with an empty expression: `albums`;
- All Tracks: `albums`.

An invalid saved-List expression falls back to the `albums` recommendation and does not change source membership or error state. If the List record cannot be found, resolution still checks the requested List's own preference, but uses the All Tracks recommendation when it needs a fallback; this does not make the missing List navigable.
`Manual Order` is an explicit user preference rather than a recommendation inferred from List storage.
The New Playlist template selects that preference when it creates a tag-backed List.

## Commands and transitions

Ordinary plain-list navigation in GTK, TUI, WinUI, and AppKit submits the preferred or recommended presentation to `WorkspaceService` as `NewViewDefault` intent.
Resolving a default does not write or clear a preference, even when the stored id is unavailable.
`WorkspaceService` alone decides whether an existing unfiltered view is reusable.
Reuse ignores that default and retains the view's exact active presentation; creation applies the resolved default to the new plain view.
A filtered view over the same list is not a reusable plain target, so `WorkspaceService` creates the plain view with the resolved default and does not alter the filtered view.

A restored workspace view retains its exact presentation; Back and Forward also restore the recorded spec, even if the picker would not offer it today. Neither operation consults or edits the preference for that view. For fallback navigation when a view or List cannot be restored, see [workspace navigation](../workspace/navigation.md).
GTK playback-session restoration uses `NewViewDefault` for its plain target. [Revealing the currently playing track](../workspace/navigation.md#navigate-to-the-playback-source) likewise resolves a default only for a newly created plain destination.
A normal named choice installs the runtime spec before the frontend updates the base List's preference. In WinUI, a rejected [presentation-menu choice](../frontend/windows.md#commands-and-transitions) reports the error and leaves the view, history, and preference unchanged.
When a committed `LibraryChangeSet` deletes Lists, the shared preference lifecycle erases every corresponding key before a frontend persists its next state.
A full library reset carries no incremental List ids, so it clears the complete preference map and emits every removed key.
That lifecycle covers only deletions the owner observed while it was alive, so `restore()` takes the library's live list ids and drops every preference naming a list that no longer exists; a virtual list id is retained without appearing among them.
Because a library never reuses a list id, that pruning also retires preferences for lists a restore replaced; a document saved for another library keeps only its virtual-list preferences when loaded, since that library's ids may name unrelated lists here.

[WinUI startup without a restored view](../frontend/windows.md#commands-and-transitions) uses `NewViewDefault` for All Tracks. Its presentation menu records the accepted spec in workspace history before pinning it; a temporary column sort does neither.
A [WinUI List editor save](../frontend/windows-library-workflows.md#create-and-edit-lists) has a different commit order: it updates the preference after the List mutation succeeds, then navigates with an `Override` carrying the resolved spec. Auto clears the pin and supplies the recommendation; even a reused view receives that spec. A navigation failure does not undo the List save or the preference change.

Applying a quick filter changes the active source and filter text, not the active presentation or the preference. It does not rerun recommendation.

## Failure and cancellation

An unknown or removed presentation id is recoverable and selects the recommendation path.
An empty built-in catalog may produce an empty fallback spec; ordinary application composition supplies the built-in catalog.

UIModel preference operations, recommendation, deletion cleanup, and persistence conversion are synchronous and have no cancellation point.
An unsupported version, missing or malformed library id, duplicate or invalid list id, empty id, or structural mismatch rejects the complete persisted preference group and preserves the caller's seeded state.
An unknown nonempty presentation id remains a valid extensible reference and follows recommendation fallback.
Frontend load/save failures do not mutate library list records.
Bulk installation during GTK restore suppresses persistence callbacks, so loading one valid sibling group cannot rewrite another rejected group.
TUI likewise installs both startup candidates before connecting save observers; rejection of one group leaves its seeded value while a valid sibling still loads.

## Persistence and versioning

GTK persists the preference map with other per-library track-view layout state through `GtkLayoutStateStore` in the library-specific `gtk_layout.yaml` store.
TUI persists the same semantic group with terminal column-layout state through its library-specific `tui_layout.yaml` store.
AppKit persists the same group through the runtime-owned `appkit-workspace.yaml` store, alongside workspace and playback-session groups.
It borrows that exact `ConfigStore` instance so checkpoints by distinct group owners preserve their cached sibling state.
Its catalog is built after workspace restoration; the map is restored against live Lists before save observers connect.
When the group fails to load, AppKit keeps that session's preference changes in memory and never saves over the rejected group.
The native catalog menu pins only accepted normal choices, Auto clears the key and applies the resulting recommendation, and a temporary header sort changes neither the map nor its save boundary.
The menu keeps the shared separator between built-in and custom presets, checks only the stored choice, and defers rebuilding while it is open.
Unavailable ids stay opaque and visible in the menu while new views use recommendation fallback; picker-less List edits do not write them.
Workspace/history replay retains exact specs, and live List deletion retires the corresponding persisted entries through shared `ListPresentations`.

WinUI persists the group with desktop column-layout state in the library-specific `winui_layout.yaml` store, keeps opaque ids across window/session replacement, and constructs the shared committed-List deletion lifecycle for each new active session.
It loads that store when the runtime graph binds rather than at startup, because the list ids the group is keyed by only mean anything against an open library.
The `trackView.presentations` group carries required `version: 1` and the `libraryId` it was saved for, and represents the map as a sequence of `{listId, presentationId}` entries so duplicate identities can be rejected before map construction.
The exact fields belong to the [persisted presentation-state reference](../../reference/presentation/persisted-state.md); group registration belongs to the [application managed-state surface](../../reference/persistence/application-config.md#group-registry).

The persisted value is a presentation id, so changing or removing a built-in id requires a compatibility path.
Unknown custom ids remain tolerated because custom presentations may be removed independently; fallback resolution never rewrites that opaque value.
Unversioned legacy preference maps and unsupported future versions are rejected without migration or automatic rewrite.
The explicit schema returns `NotSupported` for a future version before interpreting its preferences.

TUI constructs one catalog and preference model after workspace restoration, so restored custom ids resolve before list navigation. Exact restored active views remain authoritative; only fallback or later plain-list navigation consults the preference map.

## Frontend observations

Presentation pickers resolve labels and specs through the shared catalog.
The active view observes the selected `TrackPresentationSpec`; frontends do not read or write list storage to remember the choice.

Quick-filter controls and List editors may display the current presentation, but filter editing remains independent from preference mutation.

The GTK Saved-List editor shows an unavailable stored id as an explicit selected option, labeled with the retained opaque id, while list opening uses recommendation fallback. A rename or filter-only edit preserves that preference; only choosing another presentation replaces it, and choosing Auto clears it. The editor's option catalog is a construction-time snapshot, so display and save use the same identities even if the workspace presets change while the dialog is open. Opening the List resolves the retained id through the live catalog, which may now contain a changed preset or require fallback.

The TUI and AppKit editors have no presentation picker, so their saves never write the preference map: a new List stays Auto and an edit keeps whatever the List already stores, including a dangling opaque id.

## Implementation map

- [`ListPresentations`](../../../app/include/ao/uimodel/library/presentation/ListPresentations.h) owns the map, resolution order, and cleanup from `listsDeleted`.
- [`ListPresentationPreferenceYamlSchema`](../../../app/include/ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h) owns explicit YAML mapping, the versioned document, and semantic conversion.
- [`recommendListPresentation`](../../../app/include/ao/uimodel/library/presentation/ListPresentations.h) owns source-aware fallback policy.
- [`TrackPresentationCatalog`](../../../app/include/ao/uimodel/library/presentation/TrackPresentationCatalog.h) resolves built-in and custom ids.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) owns active presentation state.
- [`WorkspaceService`](../../../app/include/ao/rt/WorkspaceService.h) owns view navigation snapshots and replay under the [workspace navigation specification](../workspace/navigation.md).
- [`GtkLayoutStateStore`](../../../app/linux-gtk/app/GtkLayoutStateStore.h) owns GTK per-library serialization; [`LayoutStateStore`](../../../app/tui/LayoutStateStore.h) and [`LibraryController`](../../../app/tui/LibraryController.h) own the TUI file and navigation adaptation; WinUI [`LibrarySession`](../../../app/windows-winui/app/LibrarySession.h) owns its retained preference store and platform settings checkpoint, while [`PresentationButtonComponent`](../../../app/windows-winui/layout/component/track/TrackComponents.cpp) adapts the shared picker model.

## Test map

- [`ListPresentationsTest.cpp`](../../../test/uimodel/library/presentation/ListPresentationsTest.cpp) proves map behavior, resolution, fallbacks, and cascade cleanup.
- [`ListPresentationPreferenceYamlSchemaTest.cpp`](../../../test/uimodel/library/presentation/ListPresentationPreferenceYamlSchemaTest.cpp) proves version gates, opaque ids, and whole-group rejection.
- [`ListPresentationRecommendationTest.cpp`](../../../test/uimodel/library/presentation/ListPresentationRecommendationTest.cpp) proves source-aware recommendations.
- [`GtkLayoutStateStoreTest.cpp`](../../../test/linux-gtk/app/GtkLayoutStateStoreTest.cpp) and [`LayoutStateStoreTest.cpp`](../../../test/tui/LayoutStateStoreTest.cpp) prove independent per-library persistence.
- [`LibraryControllerTest.cpp`](../../../test/tui/LibraryControllerTest.cpp) proves TUI preference/recommendation navigation and exact restored-view precedence.
- [`MainWindowSessionPresentationTest.cpp`](../../../test/linux-gtk/app/MainWindowSessionPresentationTest.cpp) proves GTK creation, reuse, workspace restoration, history replay, and playback-restoration precedence.
- [`AppKitPresentationScenario.mm`](../../../test/macos-appkit/AppKitPresentationScenario.mm) checks native-session preference writes, reuse/new-view defaults, Auto, opaque and custom ids, deletion, history, reopen sibling preservation, and no overwrite of a rejected group; [`AppKitDesktopScenario.mm`](../../../test/macos-appkit/AppKitDesktopScenario.mm) checks native catalog identity, eligibility, label and checkmark display, open-menu deferral, and transient sort labeling.
- Workspace history tests under [`test/runtime/`](../../../test/runtime) prove snapshot replay semantics.

## Related documents

- [Presentation architecture](README.md)
- [Persistence and managed-state architecture](../persistence/README.md)
- [Track-list presentation](track-presentation.md)
- [Track filtering](track-filter.md)
- [Track presentation presets](../../reference/presentation/track-preset.md)
- [Persisted presentation state](../../reference/presentation/persisted-state.md)
- [Workspace navigation](../workspace/navigation.md)
