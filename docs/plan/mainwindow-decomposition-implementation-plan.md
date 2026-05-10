# Linux-GTK Shell Containerization Implementation Plan

Date: 2026-05-07
Status: Draft

## Goal

Turn the current `app/linux-gtk` UI into a clearer `MainWindow` shell + autonomous widget/controller architecture without changing the intended user-visible behavior.

The target end state is:

- `MainWindow` is responsible for top-level layout assembly, window-level actions, and persistence wiring.
- Feature widgets and controllers subscribe to runtime state directly where possible.
- List and view membership semantics are owned by runtime code, not reconstructed ad hoc in GTK.
- Smart-list preview, open-view membership, and nested list behavior all use the same runtime source semantics.

This plan is intentionally staged so the work can land in small, reviewable pull requests while preserving a buildable tree after each step.

## Scope

This plan covers the current Linux GTK application under `app/linux-gtk/`.

It focuses on these areas:

- `app/linux-gtk/ui/MainWindow.*`
- `app/linux-gtk/ui/ListSidebarController.*`
- `app/linux-gtk/ui/TrackPageGraph.*`
- `app/linux-gtk/ui/InspectorSidebar.*`
- `app/linux-gtk/ui/CoverArtWidget.*`
- `app/linux-gtk/ui/StatusBar.*`
- `app/linux-gtk/ui/SmartListDialog.*`
- `app/runtime/*`
- `include/ao/model/*`
- `lib/model/*`

It does not attempt to redesign the GTK visual language or change existing interaction patterns beyond what is required to make the architecture coherent.

## Current Architecture Snapshot

The current codebase is already significantly decomposed compared to a traditional god-window design.

The major pieces already exist:

- `MainWindow` assembles layout and still owns some cross-widget synchronization.
- `TrackPageGraph` owns page creation and visible-page lifecycle for track-list views.
- `ListSidebarController` owns the sidebar tree widget and list CRUD UI.
- `PlaybackBar` and `StatusBar` already self-wire to parts of `AppSession`.
- `InspectorSidebar` already binds a focused detail projection for some data.
- `ImportExportCoordinator` already owns import/export dialogs and background work.
- `TagEditController` already owns tag popovers and tag mutation actions.

The remaining architectural debt is concentrated in a few seams rather than spread uniformly across the whole UI.

## Key Findings

### 1. `MainWindow` is still the cross-widget selection hub

Today, `TrackPageGraph` selection changes are routed back into `MainWindow`, and `MainWindow` still fans that change out into:

- cover art updates
- status-bar selection info
- inspector selection updates

That means `MainWindow` still acts as a UI state dispatcher rather than a thin shell.

### 2. Smart-list source semantics belong in runtime, not GTK

The stored smart-list contract already says:

- each smart list stores only its local filter
- effective membership is `parent_membership AND local_filter`
- nested smart lists chain through ancestors

This is documented in:

- `include/ao/library/ListView.h`
- `include/ao/library/ListLayout.h`

GTK preview should not invent a separate interpretation of those semantics.

### 3. Runtime list membership needs a stable app-level owner

The current runtime/model split is misleading.

Important facts:

- `FilteredTrackIdList` accepts any `TrackIdList&` source.
- `FilteredTrackIdList` auto-registers itself with `SmartListEngine` in its constructor.
- `SmartListEngine` batches and updates lists by source bucket, not by a special all-tracks-only path.
- `ManualTrackIdList` already observes its source and forwards resets, updates, and removals.
- `SmartListEngine` does not materialize all persisted smart lists from LMDB; it only coordinates currently constructed `FilteredTrackIdList` instances.

This means nested source chains can be implemented by composition rather than by pre-combining filter expressions, but ownership should live in one runtime store instead of being attached to individual views.

The desired model is:

- list source objects are materialized on demand
- one runtime source exists per persisted list while that list still exists
- views and projections borrow stable sources
- closing or navigating away from a view does not destroy its list source
- deleting a list is the normal point where its source is destroyed

### 4. There are a few concrete correctness risks in the current runtime wiring

These should be treated as real bugs or near-bugs, not just refactor cleanup:

- `ViewService` fallback creation of `FilteredTrackIdList` manually calls `registerList()` even though `FilteredTrackIdList` already auto-registers.
- That same fallback branch constructs a filtered list without an explicit `reload()`, which risks an empty initial membership until some later source notification happens.
- `TrackDetailProjection` currently follows focus and selection changes, but it does not automatically rebuild when the selected tracks mutate in place.
- List CRUD refreshes GTK pages and sidebar state, but runtime view-source state is not yet refreshed through one coherent mutation path.

### 5. List identity still needs to be normalized

The GTK layer and runtime layer currently treat “All Tracks” through slightly different conventions.

This needs to be normalized through a single helper or shared rule before deeper shell/container work continues, otherwise every stage keeps carrying special cases.

## Design Principles

Apply these principles throughout the implementation:

1. Keep each pull request buildable.
2. Prefer reusing the existing runtime and model abstractions over introducing new shell-level controllers.
3. Move logic to the smallest correct owner instead of building a new mediator.
4. Favor direct subscription to projections and model observers over callback fan-out through `MainWindow`.
5. Preserve persisted list semantics. Do not change the on-disk meaning of smart-list `filter()`.
6. Treat nested smart-list behavior as part of correctness, not as optional polish.
7. Add tests for behavioral seams and regressions, not for trivial data plumbing.

## Target End State

At the end of this plan:

- `MainWindow` owns layout, top-level widgets, menu actions, and session/persistence hooks.
- `ListSidebarController` drives navigation directly through `WorkspaceService` and keeps its own selection in sync with focused runtime state.
- Runtime owns the canonical list-to-membership resolution path through a stable list-source store.
- `ViewService` creates views on demand and borrows list sources instead of owning per-view source chains.
- `SmartListDialog` preview uses the same runtime source resolution path as normal view opening.
- `CoverArtWidget`, `InspectorSidebar`, and `StatusBar` bind directly to focused-view runtime projections or list observers.
- `TrackPageGraph` no longer uses `MainWindow` as a selection relay.

## Pull Request Plan

Recommended sequence:

1. Runtime list-source store and smart-list semantics cleanup.
2. List mutation pipeline and sidebar/runtime refresh coherence.
3. Projection-first right-side and bottom-bar widgets.
4. `MainWindow` shell cleanup and dead-code removal.

This order matters.

The list-source store must be correct first, otherwise the shell refactor will only move broken semantics into different classes.

## PR 1: Runtime List-Source Store

### Objective

Move list membership ownership into runtime and make it stable across view lifetimes.

This is the foundation for:

- nested smart-list correctness
- consistent smart-list preview
- coherent open-view refresh after list edits
- reduced GTK-side special casing
- back/forward navigation that does not rebuild list membership unnecessarily

### Why This Comes First

The existing `lib/model` layer already has the pieces:

- `FilteredTrackIdList`
- `ManualTrackIdList`
- `SmartListEngine`
- `AllTrackIdsList`

However, these classes are not really standalone library model abstractions anymore. They are runtime membership sources used by views, projections, smart-list preview, and list mutation refresh.

What is missing is a single runtime owner that resolves a `ListId` into a stable source and keeps that source alive independently of any one view.

### Main Changes

Move the list-membership classes out of `lib/model` and into `app/runtime`, then introduce a runtime-side list-source store.

Recommended shape:

```cpp
class ListSourceStore final
{
public:
  ListSourceStore(ao::library::MusicLibrary& library);

  ao::runtime::TrackSource& allTracks();
  ao::runtime::TrackSource& sourceFor(ao::ListId listId);

  void reloadAllTracks();
  void refreshList(ao::ListId listId);
  void eraseList(ao::ListId listId);

private:
  ao::library::MusicLibrary& _library;
  AllTracksSource _allTracks;
  SmartListEvaluator _smartEvaluator;
  std::unordered_map<ao::ListId, std::unique_ptr<TrackSource>> _sources;
};
```

The exact class names can follow local style, but the ownership rule should be explicit:

- `AllTracksSource` is created with the app session.
- Persisted list sources are created on demand.
- Once a persisted list source is created, it remains alive until that list is deleted or the app session ends.
- Views and projections do not own list sources; they keep references or pointers whose lifetime is guaranteed by `ListSourceStore`.

The store should follow these rules:

- All Tracks resolves directly to `AllTracksSource`.
- Manual lists resolve to a runtime manual source backed by the persisted list and constrained by the parent source.
- Smart lists resolve to a runtime smart source backed by the parent source and local filter, followed by an initial reload.
- Nested smart lists recurse through the parent chain instead of flattening the stored filter into one combined expression.
- Parent sources must be materialized before child sources.
- Source object addresses must remain stable while borrowed by projections and adapters.
- Destroying sources must happen child-before-parent when a subtree is deleted.

### Required Runtime Changes

- Update `AppSession` to own `ListSourceStore` instead of exposing `AllTrackIdsList` and `SmartListEngine` as separate cross-layer primitives.
- Update `ViewService::createView()` to ask `ListSourceStore` for the list source whenever the caller provides a `listId`.
- Remove the `std::shared_ptr<TrackIdList>` source parameter from `ViewService::createView()` unless a concrete non-list-backed use case still requires it.
- Stop storing list-source ownership in `ViewService::ViewEntry`; view entries should own view state and projections, not list sources.
- Keep list-backed view `listId` stable. Ordinary sidebar navigation should focus an existing list view or create a new one, not replace the list inside an existing view.
- Normalize “All Tracks” identity through one helper instead of mixing sentinel logic informally.
- Start moving or renaming the old model classes so UI/runtime code no longer depends on `ao::model::TrackIdList` as a library-layer abstraction.

### PR 1 Staging Guidance

This PR can become large if the move from `lib/model` to `app/runtime` is done as one big-bang rename.

Prefer an incremental transition:

1. Introduce `ListSourceStore` in runtime while it still wraps or reuses the existing `ao::model` classes.
2. Route `ViewService`, smart-list preview, and GTK membership lookups through `ListSourceStore` first.
3. Once the runtime ownership boundary is proven by tests, move or rename the old model classes into runtime in a follow-up slice or as the final step of PR 1.
4. Avoid maintaining two independent implementations; compatibility shims are acceptable temporarily, but the runtime store should become the single construction path.

The important checkpoint is not the physical file move by itself. The important checkpoint is that list sources are owned by the runtime store, materialized on demand, reused across view close/refocus, and erased only when the persisted list is deleted or the app session ends.

### Explicit Fixes To Include

Fix these current issues while touching the runtime path:

- Remove the duplicate manual `registerList()` call in the fallback branch of `ViewService` if that fallback still exists during migration.
- Ensure query-backed or fallback smart-source instances receive an initial `reload()`.
- Add tests that fail if either regression returns.

### Files Likely Touched

- `app/runtime/ViewService.h`
- `app/runtime/ViewService.cpp`
- `app/runtime/AppSession.h`
- `app/runtime/AppSession.cpp`
- new runtime source classes such as `app/runtime/TrackSource.*`, `app/runtime/AllTracksSource.*`, `app/runtime/ManualListSource.*`, `app/runtime/SmartListSource.*`
- new runtime owner such as `app/runtime/ListSourceStore.*`
- smart-list evaluator code moved from `lib/model/SmartListEngine.*` or renamed in runtime, either in this PR or in the follow-up slice after `ListSourceStore` becomes the only construction path
- old `include/ao/model/*TrackIdList*.h` and `lib/model/*TrackIdList*.cpp` paths removed or reduced to compatibility shims only after runtime callers have moved to `ListSourceStore`
- possibly `app/runtime/WorkspaceService.cpp`
- model/runtime tests covering list resolution and initial view population

### Tests

Add tests for:

- root smart list membership
- manual-to-smart source chaining
- smart-to-smart source chaining
- initial fallback all-tracks or query-backed view population
- duplicate-registration regression
- closing a view does not destroy or rebuild the cached list source
- reopening or refocusing a list reuses the same runtime source while the list still exists
- deleting a list closes affected views before erasing its source

### Done Criteria

- Opening a nested smart list in the UI uses the correct chained membership semantics.
- Runtime view creation no longer depends on GTK-specific source guessing or per-view source ownership.
- `ViewService` no longer double-registers filtered lists.
- Initial filtered-list-backed views populate immediately.
- List-backed sources live independently of view lifetimes and are released when their persisted list is deleted or the session ends.
- Each bullet in the PR 1 test list is represented by a focused runtime test or an explicitly documented GTK-only manual verification.

## PR 2: List Mutation Pipeline And Sidebar Coherence

### Objective

Make list create/edit/delete flow coherent across sidebar UI, runtime view state, and already-open track pages.

### Problem Being Solved

Today list mutations mainly rebuild GTK-side structures. That is not enough.

Open runtime views also need their underlying borrowed sources refreshed after list definitions change.

### Main Changes

Add a dedicated runtime event for list definition changes.

Suggested event:

```cpp
struct ListsMutated final
{
  std::vector<ao::ListId> upserted{};
  std::vector<ao::ListId> deleted{};
};
```

Then update the flow as follows:

- `ListSidebarController` invokes a runtime mutation API instead of publishing mutation events itself.
- The runtime mutation owner writes list mutations to the library and publishes `ListsMutated` after the write succeeds.
- `ListSidebarController` rebuilds its own tree.
- `ListSourceStore` listens for or receives `ListsMutated` and refreshes any materialized source affected by those list definitions.
- `WorkspaceService` closes open views that target deleted lists before `ListSourceStore` erases the deleted sources.
- `TrackPageGraph` refreshes stack titles when list names change.

The event publication boundary should stay in runtime, not GTK. GTK controllers may initiate a list edit, but they should not be the canonical publisher because future CLI, import, or batch operations can mutate list definitions without going through `ListSidebarController`.

### Sidebar Boundary Changes

Move the sidebar closer to autonomous operation:

- Selection changes should navigate directly through `_session.workspace().navigateTo(...)`.
- Sidebar selection should also track focused runtime state instead of depending on `MainWindow` to re-select items.
- Remove or shrink callback plumbing that only exists to bounce list selection through `MainWindow`.

### Smart-List Preview Changes

`SmartListDialog` must stop accepting `parentMembershipList` as an externally supplied GTK-side concept.

Instead:

- pass `parentListId`
- resolve the parent source using the same runtime `ListSourceStore` path as normal views
- display inherited and effective expression labels from that resolved source context
- keep persisted data as local expression only

This ensures preview and actual view opening use one semantic path.

### Files Likely Touched

- `app/runtime/EventTypes.h`
- `app/runtime/LibraryMutationService.*` or another runtime list-mutation owner
- `app/runtime/ListSourceStore.*`
- `app/runtime/ViewService.cpp`
- `app/runtime/WorkspaceService.cpp`
- `app/linux-gtk/ui/ListSidebarController.h`
- `app/linux-gtk/ui/ListSidebarController.cpp`
- `app/linux-gtk/ui/TrackPageGraph.cpp`
- `app/linux-gtk/ui/SmartListDialog.h`
- `app/linux-gtk/ui/SmartListDialog.cpp`

### Tests

Add tests for:

- editing a list refreshes the materialized source and updates open-view membership
- renaming a list updates the visible page title
- deleting an open list closes or redirects the corresponding view cleanly
- smart-list preview matches runtime-opened membership
- sidebar selection follows focused view correctly

### Done Criteria

- List CRUD no longer depends on `MainWindow` or GTK-only event publication to coordinate runtime refresh.
- Open views stay consistent after list edits.
- Sidebar selection and workspace focus stay in sync.
- Smart-list preview uses the same membership semantics as runtime view creation.
- Each PR 2 behavior has a named runtime or GTK regression test, except any GTK-only manual check explicitly listed in the PR notes.

## PR 3: Projection-First Cover, Inspector, And Status Widgets

### Objective

Remove the last major selection fan-out logic from `MainWindow` by making the cover art, inspector, and selection status widgets bind directly to runtime state.

### Problem Being Solved

`MainWindow` still acts as the selection dispatcher.

That is the biggest remaining obstacle to a true shell/container role.

### Main Changes

Extend `TrackDetailSnapshot` so it contains the metadata the inspector truly needs.

Suggested additions:

- aggregated title
- aggregated artist
- aggregated album
- possibly future fields such as year or album artist if needed

Then update `TrackDetailProjection` to rebuild not only on focus and selection changes, but also when the currently selected tracks are mutated.

That means `TrackDetailProjection` should subscribe to `TracksMutated` and refresh only when the current snapshot selection intersects the mutated ids.

Keep the rebuild path transaction-aware. If `buildSnapshot` still opens a separate read transaction per selected track, a large batch tag edit can make `TracksMutated` a hot path. The first implementation can keep the existing shape if tests and profiling do not show a problem, but the plan should allow consolidating selected-track reads into one transaction when touching this code.

### Widget Migration

After projection refresh is correct:

- `InspectorSidebar` should become fully projection-first.
- `CoverArtWidget` should bind to the focused detail projection instead of raw `ViewSelectionChanged`.
- `StatusBar` selection info should bind to focused detail projection instead of being pushed from `MainWindow`.
- Track-count display should bind to `allTracks()` through an observer or equivalent runtime hook.

### Important Inspector Note

`InspectorSidebar` should stop depending on a cached vector of `TrackRow` objects for displayed state.

For edit operations:

- use the projected `trackIds`
- call mutation services
- rely on `TracksMutated` plus projection refresh for UI updates

This removes the need for `MainWindow` to push current selection rows into the inspector.

### `MainWindow` Cleanup After Migration

Once the widgets are projection-first, delete these shell-side responsibilities:

- `MainWindow::updateCoverArt(...)`
- `MainWindow::onTrackSelectionChanged()`
- selection fan-out callbacks passed into `TrackPageGraph` that only forward to cover/status/inspector

### Files Likely Touched

- `app/runtime/ProjectionTypes.h`
- `app/runtime/TrackDetailProjection.cpp`
- `app/linux-gtk/ui/InspectorSidebar.h`
- `app/linux-gtk/ui/InspectorSidebar.cpp`
- `app/linux-gtk/ui/CoverArtWidget.h`
- `app/linux-gtk/ui/CoverArtWidget.cpp`
- `app/linux-gtk/ui/StatusBar.h`
- `app/linux-gtk/ui/StatusBar.cpp`
- `app/linux-gtk/ui/MainWindow.h`
- `app/linux-gtk/ui/MainWindow.cpp`
- tests for projection refresh and widget behavior

### Tests

Add tests for:

- detail projection refresh on `TracksMutated`
- cover art follows active view rather than background selection events
- inspector refreshes after metadata edits without reselection
- status selection count refreshes from projection state
- track count refreshes from the library list observer

### Done Criteria

- Cover art, inspector, and selection status no longer require `MainWindow` as a relay.
- Editing selected tracks updates the inspector through projection refresh.
- `MainWindow` no longer owns selection fan-out logic.
- Projection refresh after `TracksMutated` is covered by a focused runtime test, including a non-intersecting mutation case that does not rebuild.
- If the projection rebuild implementation changes transaction behavior, the relevant test or PR notes should state whether selected-track reads are batched in one transaction or intentionally left as-is.

## PR 4: `MainWindow` Shell Cleanup

### Objective

Finish the transition by stripping `MainWindow` down to its real shell responsibilities and removing dead seams left behind by the earlier work.

### Main Changes

At this stage, `MainWindow` should mainly do the following:

- construct top-level widgets and controllers
- build the top-level GTK layout
- install window-level actions
- connect persistence and session restore hooks

The cleanup work should include:

- remove no-longer-needed `TrackPageGraph` callbacks
- remove now-unused helper methods from `MainWindow`
- remove dead selection-context fields that are no longer consumed
- remove dead inspector signal plumbing if it still exists but has no emit path
- move child-widget-specific CSS out of `MainWindow` and into the owning widgets
- update any stale persistence comments so they reflect the real flow

### Specific Candidates For Removal

These should be reviewed once earlier phases land:

- `MainWindow::showListPage(...)` if direct workspace navigation makes it redundant
- `MainWindow::currentSelectionPlaybackDescriptor()` if still unused
- `TrackSelectionContext::membershipList` if runtime no longer requires it
- `TrackPageContext::membershipList` if it remains a dead field
- any `MainWindow` callback wiring whose only purpose was forwarding between widgets

### Files Likely Touched

- `app/linux-gtk/ui/MainWindow.h`
- `app/linux-gtk/ui/MainWindow.cpp`
- `app/linux-gtk/ui/TrackPageGraph.h`
- `app/linux-gtk/ui/TrackPageGraph.cpp`
- `app/linux-gtk/ui/TagEditController.h`
- `app/linux-gtk/ui/InspectorSidebar.*`
- possibly `app/linux-gtk/ui/SessionPersistence.cpp`

### Tests

Focus on regression coverage for end-to-end UI behavior already protected by earlier phases.

The value of this phase is simplification, not new behavior.

### Done Criteria

- `MainWindow` reads as a shell/container rather than a feature coordinator.
- Widget-specific styling and logic live with the relevant widgets.
- Dead callback seams and unused helper methods are removed.

## Validation Checklist After Each PR

After each phase, validate at least the following:

- Build succeeds.
- Existing library opens correctly.
- Import still populates the UI and updates track counts.
- Sidebar navigation switches content correctly.
- Smart-list preview matches opened list behavior.
- Nested smart lists behave correctly.
- Tag editing still mutates selected tracks.
- Playback start, pause, seek, stop, and reveal-playing-track still work.
- Session restore still brings back the expected views.

## Test Strategy

This repository expects meaningful automated coverage for behavior changes.

The highest-value tests for this plan are:

- runtime tests around list-source-store resolution and nested smart-list semantics
- runtime tests around materialized source refresh after list mutation
- runtime tests around list-source lifetime across view close/refocus
- runtime tests around projection refresh after `TracksMutated`
- one or two GTK-layer regression tests for shell-facing behavior where runtime tests are insufficient

For reviewability, each PR should map its Done Criteria to concrete verification: a test file/test case name for automated coverage, or a short manual-check note when the behavior is GTK-only and not practical to automate in that PR.

Avoid spending effort on low-value tests for thin forwarding methods or simple data holders.

## Risk Register

### Risk 1: Nested smart-list semantics drift during refactor

Mitigation:

- lock behavior down with runtime tests before changing `ViewService`
- keep stored filter semantics unchanged

### Risk 2: Open views hold dangling references when list sources are erased or rebuilt

Mitigation:

- make `ListSourceStore` own stable source objects and keep them alive independently of view lifetimes
- close affected views before erasing deleted list sources
- refresh source nodes in place whenever possible so projections remain attached to stable objects

### Risk 3: Projection-first inspector stops updating after edits

Mitigation:

- add explicit `TracksMutated` handling to `TrackDetailProjection`
- test edit-without-reselection behavior

### Risk 4: Sidebar and workspace focus diverge for query-only views

Mitigation:

- define one explicit rule for non-list-backed views
- recommended rule: no list selection is highlighted for purely ad hoc query views

## Non-Goals

The following are intentionally out of scope for this plan unless they become necessary to preserve behavior:

- redesigning GTK visual styling
- introducing a new generic UI framework layer
- changing persisted list payload formats
- replacing quick filter behavior with a fully runtime-backed query-view model
- adding new inspector features unrelated to shell/container decomposition

## End-State Responsibility Split

When this plan is complete, responsibilities should look like this:

### `MainWindow`

- top-level layout
- top-level action registration
- subcomponent construction
- persistence hooks

### `ListSourceStore`

- create and own canonical runtime list sources
- keep materialized list sources alive until their persisted list is deleted or the session ends
- refresh materialized sources after list definition changes

### `ViewService`

- create view state and projections on demand
- borrow canonical list sources from `ListSourceStore`
- expose projections for GTK consumers

### `ListSidebarController`

- own sidebar widget tree and list CRUD UI
- drive workspace navigation directly
- stay in sync with focused runtime state

### `TrackPageGraph`

- create and manage GTK page widgets for runtime views
- keep stack membership and visible page state coherent
- update page titles when list metadata changes

### `InspectorSidebar`, `CoverArtWidget`, `StatusBar`

- consume runtime projections or observers directly
- update themselves without a `MainWindow` relay

### `SmartListDialog`

- preview from the canonical `ListSourceStore` source resolution path
- persist only local filter expressions

## Expected Result

After these four pull requests:

- `MainWindow` is no longer the implicit synchronization center of the UI.
- GTK and runtime agree on list membership semantics.
- Smart-list preview and actual opened-view behavior match.
- The remaining architecture is simpler, more local, and easier to extend safely.
