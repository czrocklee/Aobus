// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "test/runtime/ViewServiceTestSupport.h"
#include "test/runtime/WorkspaceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/Signal.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::test;

  namespace
  {
    constexpr auto kMatchingTrackFilter = R"($title = "Shared Song")";

    struct TrackSourceNavigationFixture final
    {
      TempDir tempDir;
      std::unique_ptr<AppRuntime> runtimePtr = makeRuntime(tempDir, std::make_unique<async::LoopExecutor>());
      ListId sourceListId = createList("Source A", "#source");
      ListId otherListId = createList("Other B", "#other");
      TrackId trackId = addRuntimeTrack(*runtimePtr, {.title = "Shared Song", .tags = {"source", "other"}});

    private:
      ListId createList(std::string name, std::string expression) const
      {
        auto& runtime = *runtimePtr;
        auto draft = ListDraft{.name = std::move(name), .expression = std::move(expression)};
        return requireValue(runRuntimeTask(runtime, runtime.library().commands().createListAsync(std::move(draft))));
      }
    };

    TrackPresentationSpec presetSpec(std::string_view id)
    {
      auto const* preset = builtinTrackPresentationPreset(id);
      REQUIRE(preset != nullptr);
      return normalizeTrackPresentationSpec(preset->spec);
    }

    bool hasVisibleTrack(AppRuntime& runtime, ViewId viewId, TrackId trackId)
    {
      auto const projectionPtr = requireValue(runtime.views().findTrackListProjection(viewId));
      REQUIRE(projectionPtr != nullptr);
      return projectionPtr->indexOf(trackId).has_value();
    }
  } // namespace

  TEST_CASE("WorkspaceService - first navigate opens the target list")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));

    auto const layout = runtime.workspace().snapshot();
    CHECK(layout.activeViewId != kInvalidViewId);
    auto const state = runtime.views().trackListState(layout.activeViewId);
    CHECK(state.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - navigate AllTracks opens the global list")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));
    REQUIRE(runtime.workspace().navigate({.target = GlobalViewKind::AllTracks}));

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == kAllTracksListId);
  }

  TEST_CASE("WorkspaceService - filtered AllTracks navigation uses the global list")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    REQUIRE(runtime.workspace().navigate({
      .target =
        FilteredListTarget{
          .listId = kAllTracksListId,
          .filterExpression = "$genre = \"Rock\"",
        },
    }));

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.filterExpression == "$genre = \"Rock\"");
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource prefers the saved source over the active list")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = *fixture.runtimePtr;
    auto& workspace = runtime.workspace();
    auto const existingPresentation = presetSpec("songs");
    auto const reopenPresentation = presetSpec("albums");
    auto const previousPresentation = presetSpec("artists");
    auto const sourceViewId =
      requireNavigation(runtime,
                        NavigationRequest{.target = fixture.sourceListId,
                                          .optPresentation = NavigationPresentation{.spec = existingPresentation}});
    REQUIRE(hasVisibleTrack(runtime, sourceViewId, fixture.trackId));
    auto const previousViewId = requireNavigation(
      runtime,
      NavigationRequest{
        .target = FilteredListTarget{.listId = fixture.otherListId, .filterExpression = kMatchingTrackFilter},
        .recordHistory = false,
        .optPresentation = NavigationPresentation{.spec = previousPresentation},
      });
    REQUIRE(hasVisibleTrack(runtime, previousViewId, fixture.trackId));

    auto const sourceClosed = GENERATE(false, true);
    CAPTURE(sourceClosed);

    if (sourceClosed)
    {
      REQUIRE(workspace.closeView(sourceViewId));
    }

    auto presentationRequests = std::vector<ListId>{};
    auto const defaultPresentation = [&](ListId listId)
    {
      presentationRequests.push_back(listId);
      return TrackPresentationSpec{reopenPresentation};
    };

    auto const viewId =
      requireValue(workspace.navigateToTrackSource(fixture.trackId, fixture.sourceListId, defaultPresentation));

    CHECK(presentationRequests == (sourceClosed ? std::vector{fixture.sourceListId} : std::vector<ListId>{}));
    CHECK(workspace.snapshot().activeViewId == viewId);
    CHECK((viewId != sourceViewId) == sourceClosed);
    auto const state = runtime.views().trackListState(viewId);
    CHECK(state.listId == fixture.sourceListId);
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation == (sourceClosed ? reopenPresentation : existingPresentation));
    CHECK(state.selection.empty());
    CHECK(hasVisibleTrack(runtime, viewId, fixture.trackId));

    // The active B view was not recorded when opened, so Back needs a new checkpoint for it.
    auto const backViewId = requireBackNavigation(runtime);
    CHECK(backViewId == previousViewId);
    auto const backState = runtime.views().trackListState(backViewId);
    CHECK(backState.listId == fixture.otherListId);
    CHECK(backState.filterExpression == kMatchingTrackFilter);
    CHECK(backState.presentation == previousPresentation);
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource falls back to All Tracks when the source loses the track")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = *fixture.runtimePtr;
    auto& workspace = runtime.workspace();
    auto& library = runtime.library();
    auto const sourceViewId = requireNavigation(runtime, fixture.sourceListId);
    auto const otherViewId = requireNavigation(runtime, fixture.otherListId);
    REQUIRE(hasVisibleTrack(runtime, sourceViewId, fixture.trackId));

    SECTION("The source list was deleted")
    {
      REQUIRE(runRuntimeTask(runtime, library.commands().deleteListAsync(fixture.sourceListId)));
    }

    SECTION("The track was removed from the source list")
    {
      auto targets = requireValue(library.bindTrackTargets(std::array{fixture.trackId}));
      auto const removedRes =
        runRuntimeTask(runtime, library.commands().removeTracksFromListAsync(fixture.sourceListId, std::move(targets)));
      REQUIRE(removedRes);
      settleRuntimeCallbacks(runtime);
      REQUIRE_FALSE(hasVisibleTrack(runtime, sourceViewId, fixture.trackId));
    }

    REQUIRE(hasVisibleTrack(runtime, otherViewId, fixture.trackId));
    auto const allTracksPresentation = presetSpec("albums");
    auto const defaultPresentation = [&](ListId listId)
    { return listId == kAllTracksListId ? allTracksPresentation : defaultTrackPresentationSpec(); };

    auto const viewId =
      requireValue(workspace.navigateToTrackSource(fixture.trackId, fixture.sourceListId, defaultPresentation));

    CHECK(workspace.snapshot().activeViewId == viewId);
    auto const state = runtime.views().trackListState(viewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation == allTracksPresentation);
    CHECK(hasVisibleTrack(runtime, viewId, fixture.trackId));
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource opens All Tracks for an unspecified source")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = *fixture.runtimePtr;
    auto& workspace = runtime.workspace();
    auto const departureViewId = requireNavigation(runtime, fixture.otherListId);
    REQUIRE(hasVisibleTrack(runtime, departureViewId, fixture.trackId));
    auto const before = workspace.snapshot();

    auto const viewId = requireValue(workspace.navigateToTrackSource(fixture.trackId, kInvalidListId));

    auto const after = workspace.snapshot();
    CHECK(after.activeViewId == viewId);
    CHECK(after.openViews == std::vector{departureViewId, viewId});
    CHECK(after.revision == before.revision + 1);
    auto const state = runtime.views().trackListState(viewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(state.filterExpression.empty());
    CHECK(hasVisibleTrack(runtime, viewId, fixture.trackId));
    CHECK_FALSE(workspace.canGoForward());
    REQUIRE(requireBackNavigation(runtime) == departureViewId);
    CHECK_FALSE(workspace.canGoBack());
    CHECK(requireForwardNavigation(runtime) == viewId);
  }

  TEST_CASE(
    "WorkspaceService - navigateToTrackSource reuses inactive plain All Tracks for an explicit All Tracks source")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = *fixture.runtimePtr;
    auto& workspace = runtime.workspace();
    requireNavigation(
      runtime, FilteredListTarget{.listId = kAllTracksListId, .filterExpression = kMatchingTrackFilter});
    auto const presentation = presetSpec("songs");
    auto const allTracksViewId =
      requireNavigation(runtime,
                        NavigationRequest{.target = GlobalViewKind::AllTracks,
                                          .optPresentation = NavigationPresentation{.spec = presentation}});
    auto const departureViewId = requireNavigation(runtime, fixture.otherListId);
    REQUIRE(hasVisibleTrack(runtime, departureViewId, fixture.trackId));
    auto const before = workspace.snapshot();
    bool requestedPresentation = false;
    auto const defaultPresentation = [&](ListId)
    {
      requestedPresentation = true;
      return defaultTrackPresentationSpec();
    };

    auto const viewId =
      requireValue(workspace.navigateToTrackSource(fixture.trackId, kAllTracksListId, defaultPresentation));

    CHECK(viewId == allTracksViewId);
    auto const after = workspace.snapshot();
    CHECK(after.activeViewId == allTracksViewId);
    CHECK(after.openViews == before.openViews);
    CHECK(after.revision == before.revision + 1);
    CHECK_FALSE(requestedPresentation);
    CHECK(runtime.views().trackListState(allTracksViewId).presentation == presentation);
    CHECK(hasVisibleTrack(runtime, allTracksViewId, fixture.trackId));
    CHECK_FALSE(workspace.canGoForward());
    REQUIRE(requireBackNavigation(runtime) == departureViewId);
    CHECK(requireForwardNavigation(runtime) == allTracksViewId);
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource opens a plain view when the source filter hides the track")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = *fixture.runtimePtr;
    auto& workspace = runtime.workspace();
    auto const filteredViewId = requireNavigation(runtime, fixture.sourceListId);
    REQUIRE(hasVisibleTrack(runtime, filteredViewId, fixture.trackId));
    auto const previousPresentation = presetSpec("artists");
    REQUIRE(workspace.setActivePresentation(previousPresentation, {.recordHistory = false}));
    auto const* const filter = R"($title = "Different Song")";
    REQUIRE(runtime.views().setFilter(filteredViewId, filter));
    settleRuntimeCallbacks(runtime);
    REQUIRE_FALSE(hasVisibleTrack(runtime, filteredViewId, fixture.trackId));

    auto const viewId = requireValue(workspace.navigateToTrackSource(fixture.trackId, fixture.sourceListId));

    CHECK(workspace.snapshot().activeViewId == viewId);
    CHECK(viewId != filteredViewId);
    auto const state = runtime.views().trackListState(viewId);
    CHECK(state.listId == fixture.sourceListId);
    CHECK(state.filterExpression.empty());
    CHECK(hasVisibleTrack(runtime, viewId, fixture.trackId));
    CHECK(runtime.views().trackListState(filteredViewId).filterExpression == filter);

    auto const backViewId = requireBackNavigation(runtime);
    CHECK(backViewId == filteredViewId);
    auto const backState = runtime.views().trackListState(backViewId);
    CHECK(backState.filterExpression == filter);
    CHECK(backState.presentation == previousPresentation);
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource opens a plain view despite an inactive matching filtered view")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = *fixture.runtimePtr;
    auto& workspace = runtime.workspace();
    auto& views = runtime.views();
    auto const filteredViewId = requireNavigation(
      runtime, FilteredListTarget{.listId = fixture.sourceListId, .filterExpression = kMatchingTrackFilter});
    REQUIRE(views.setSelection(filteredViewId, {fixture.trackId}));
    auto const departureViewId = requireNavigation(runtime, fixture.otherListId);
    auto const departurePresentation = presetSpec("artists");
    REQUIRE(views.setFilter(departureViewId, kMatchingTrackFilter));
    REQUIRE(workspace.setActivePresentation(departurePresentation, {.recordHistory = false}));
    settleRuntimeCallbacks(runtime);

    REQUIRE(workspace.snapshot().openViews == std::vector{filteredViewId, departureViewId});
    auto const filteredBefore = views.trackListState(filteredViewId);
    REQUIRE(filteredBefore.filterExpression == kMatchingTrackFilter);
    REQUIRE(hasVisibleTrack(runtime, filteredViewId, fixture.trackId));

    auto const viewId = requireValue(workspace.navigateToTrackSource(fixture.trackId, fixture.sourceListId));

    auto const after = workspace.snapshot();
    CHECK(after.activeViewId == viewId);
    CHECK(after.openViews == std::vector{filteredViewId, departureViewId, viewId});
    auto const state = requireValue(views.findTrackListState(viewId));
    CHECK(state.listId == fixture.sourceListId);
    CHECK(state.filterExpression.empty());
    CHECK(hasVisibleTrack(runtime, viewId, fixture.trackId));
    auto const filteredAfter = requireValue(views.findTrackListState(filteredViewId));
    CHECK(filteredAfter.filterExpression == kMatchingTrackFilter);
    CHECK(filteredAfter.presentation == filteredBefore.presentation);
    CHECK(filteredAfter.selection == std::vector{fixture.trackId});
    CHECK(hasVisibleTrack(runtime, filteredViewId, fixture.trackId));

    // The departing filter and presentation changed after the last history entry.
    REQUIRE(requireBackNavigation(runtime) == departureViewId);
    auto const backState = views.trackListState(departureViewId);
    CHECK(backState.filterExpression == kMatchingTrackFilter);
    CHECK(backState.presentation == departurePresentation);
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource keeps the active filtered source if it shows the track")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = *fixture.runtimePtr;
    auto& workspace = runtime.workspace();
    auto const viewId = requireNavigation(
      runtime, FilteredListTarget{.listId = fixture.sourceListId, .filterExpression = kMatchingTrackFilter});
    auto const forwardViewId = requireNavigation(runtime, fixture.otherListId);
    REQUIRE(requireBackNavigation(runtime) == viewId);
    REQUIRE(hasVisibleTrack(runtime, viewId, fixture.trackId));
    auto const presentation = presetSpec("artists");
    REQUIRE(workspace.setActivePresentation(presentation, {.recordHistory = false}));
    auto const before = workspace.snapshot();
    bool requestedPresentation = false;
    auto const defaultPresentation = [&](ListId)
    {
      requestedPresentation = true;
      return defaultTrackPresentationSpec();
    };

    auto const resolvedViewId =
      requireValue(workspace.navigateToTrackSource(fixture.trackId, fixture.sourceListId, defaultPresentation));

    CHECK(resolvedViewId == viewId);
    CHECK(workspace.snapshot() == before);
    CHECK_FALSE(requestedPresentation);
    auto const state = runtime.views().trackListState(viewId);
    CHECK(state.filterExpression == kMatchingTrackFilter);
    CHECK(state.presentation == presentation);
    CHECK(state.selection.empty());
    CHECK_FALSE(workspace.canGoBack());
    CHECK(requireForwardNavigation(runtime) == forwardViewId);
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource leaves workspace and source unchanged for a missing track")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = *fixture.runtimePtr;
    auto& workspace = runtime.workspace();
    auto& views = runtime.views();
    auto const viewId = requireNavigation(
      runtime, FilteredListTarget{.listId = fixture.otherListId, .filterExpression = kMatchingTrackFilter});
    auto const forwardViewId = requireNavigation(runtime, fixture.sourceListId);
    REQUIRE(requireBackNavigation(runtime) == viewId);
    REQUIRE(views.setSelection(viewId, {fixture.trackId}));
    auto const before = workspace.snapshot();
    auto const beforeProjectionPtr = requireValue(views.findTrackListProjection(viewId));
    auto const beforeRevision = runtime.library().snapshot().revision();
    auto const missingTrackId = TrackId{999999};
    REQUIRE_FALSE(runtime.library().snapshot().containsTrack(missingTrackId));
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub = workspace.onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });

    auto const res = workspace.navigateToTrackSource(missingTrackId, fixture.sourceListId);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
    settleRuntimeCallbacks(runtime);
    CHECK(workspace.snapshot() == before);
    CHECK(changes.empty());
    CHECK(views.trackListState(viewId).selection == std::vector{fixture.trackId});
    CHECK(requireValue(views.findTrackListProjection(viewId)) == beforeProjectionPtr);
    CHECK(requireValue(views.listSourceTrackIds(forwardViewId)) == std::vector{fixture.trackId});
    CHECK(runtime.library().snapshot().revision() == beforeRevision);
    CHECK_FALSE(workspace.canGoBack());
    CHECK(requireForwardNavigation(runtime) == forwardViewId);
    CHECK(requireBackNavigation(runtime) == viewId);
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource rejects an invalid inherited filter without falling back")
  {
    auto fixture = ViewServiceFixture{};
    auto& storage = fixture.libraryFixture.library();
    auto const trackId = library::test::addTrack(storage, {.title = "Shared Song"});
    auto childId = kInvalidListId;

    {
      auto transaction = library::test::writeTransaction(storage);
      auto parentBuilder = library::ListBuilder::makeEmpty().name("Invalid parent").filter("(");
      auto const parentId = requireValue(
        transaction.apply([&](library::LibraryWrite& write) { return write.lists().create(parentBuilder); }));
      auto childBuilder = library::ListBuilder::makeEmpty().name("Child").parentId(parentId);
      childId = requireValue(
        transaction.apply([&](library::LibraryWrite& write) { return write.lists().create(childBuilder); }));
      REQUIRE(transaction.commit());
    }

    fixture.cachePtr->reloadAllTracks();
    auto const viewId = fixture.requireView({.filterExpression = R"($title = "Different Song")"});
    auto const forwardViewId = fixture.requireView({.filterExpression = kMatchingTrackFilter});
    REQUIRE(fixture.requireProjection(forwardViewId)->indexOf(trackId).has_value());
    auto& workspace = fixture.workspace;
    REQUIRE(workspace.goBack());
    fixture.drainCallbacks();
    auto const before = workspace.snapshot();
    REQUIRE(before.activeViewId == viewId);
    REQUIRE(workspace.canGoForward());
    bool selectionChanged = false;
    auto const selectionSub = fixture.service.onSelectionChanged([&](ViewService::SelectionChanged const&) noexcept
                                                                 { selectionChanged = true; });
    bool workspaceChanged = false;
    auto const workspaceSub = workspace.onChanged([&](WorkspaceChanged const&) noexcept { workspaceChanged = true; });

    auto const res = workspace.navigateToTrackSource(trackId, childId);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
    fixture.drainCallbacks();
    CHECK(workspace.snapshot() == before);
    CHECK_FALSE(selectionChanged);
    CHECK_FALSE(workspaceChanged);
    CHECK_FALSE(workspace.canGoBack());
    REQUIRE(workspace.goForward());
    CHECK(workspace.snapshot().activeViewId == forwardViewId);
    CHECK_FALSE(workspace.canGoForward());
    REQUIRE(workspace.goBack());
    CHECK(workspace.snapshot().activeViewId == viewId);
    CHECK_FALSE(workspace.canGoBack());
  }

  TEST_CASE("AppRuntime - jumpToAlbum rejects invalid tracks")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));
    auto const before = runtime.workspace().snapshot();
    auto revealRequests = std::vector<PlaybackRevealTrackRequest>{};
    auto const sub = runtime.playback().events().onRevealTrackRequested(
      [&](PlaybackRevealTrackRequest const& request) noexcept { revealRequests.push_back(request); });

    auto const res = runtime.jumpToAlbum(kInvalidTrackId);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    settleRuntimeCallbacks(runtime);
    CHECK(runtime.workspace().snapshot() == before);
    CHECK_FALSE(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());
    CHECK(revealRequests.empty());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - onChanged includes the committed focus")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto focusedViewId = kInvalidViewId;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept
                                                   { focusedViewId = changed.snapshot.activeViewId; });

    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));
    settleRuntimeCallbacks(runtime);
    auto activeViewId = runtime.workspace().snapshot().activeViewId;
    CHECK(focusedViewId == activeViewId);
  }

  TEST_CASE("WorkspaceService - deleting a list closes its open views")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto listId = fixture.createList("Test List");
    REQUIRE(runtime.workspace().navigate({.target = listId}));

    auto activeViewId = runtime.workspace().snapshot().activeViewId;
    CHECK(activeViewId != kInvalidViewId);

    REQUIRE(runRuntimeTask(runtime, runtime.library().commands().deleteListAsync(listId)));

    auto layout = runtime.workspace().snapshot();
    CHECK(!std::ranges::contains(layout.openViews, activeViewId));
  }

  TEST_CASE("WorkspaceService - deleting a list closes all matching views in one commit")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    requireNavigation(runtime, FilteredListTarget{.listId = fixture.firstListId, .filterExpression = "$title ~ \"A\""});
    requireNavigation(runtime, FilteredListTarget{.listId = fixture.firstListId, .filterExpression = "$title ~ \"B\""});
    auto const before = runtime.workspace().snapshot();
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub =
      runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });

    REQUIRE(runRuntimeTask(runtime, runtime.library().commands().deleteListAsync(fixture.firstListId)));

    REQUIRE(changes.size() == 1);
    CHECK(changes.front().cause == WorkspaceChangeCause::ListDeletion);
    CHECK(changes.front().snapshot.openViews.empty());
    CHECK(changes.front().snapshot.activeViewId == kInvalidViewId);
    CHECK(changes.front().snapshot.revision == before.revision + 1);
  }

  TEST_CASE("WorkspaceService - a library restore closes the views of lists it replaced")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto closedViewIds = std::vector<ViewId>{};
    auto allTracksViewId = kInvalidViewId;

    SECTION("All Tracks is focused")
    {
      closedViewIds.push_back(requireNavigation(runtime, fixture.firstListId));
      allTracksViewId = requireNavigation(runtime, GlobalViewKind::AllTracks);
    }

    SECTION("The replaced list's view is focused")
    {
      allTracksViewId = requireNavigation(runtime, GlobalViewKind::AllTracks);
      closedViewIds.push_back(requireNavigation(runtime, fixture.firstListId));
    }

    SECTION("Several filtered views of a replaced list are open")
    {
      allTracksViewId = requireNavigation(runtime, GlobalViewKind::AllTracks);
      closedViewIds.push_back(requireNavigation(
        runtime, FilteredListTarget{.listId = fixture.firstListId, .filterExpression = "$title ~ \"A\""}));
      closedViewIds.push_back(requireNavigation(
        runtime, FilteredListTarget{.listId = fixture.firstListId, .filterExpression = "$title ~ \"B\""}));
    }

    auto const before = runtime.workspace().snapshot();
    auto const yamlPath = fixture.tempDir.path() / "backup.yaml";
    REQUIRE(runRuntimeTask(runtime, runtime.library().jobs().exportLibraryAsync(yamlPath, ExportMode::Full)));
    auto planRes =
      runRuntimeTask(runtime, runtime.library().jobs().prepareLibraryImportAsync(yamlPath, ImportMode::Restore));
    REQUIRE(planRes);
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub =
      runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });
    auto destroyed = std::vector<ViewService::ViewDestroyed>{};
    auto const destroyedSub = runtime.views().onViewDestroyed([&](ViewService::ViewDestroyed const& event) noexcept
                                                              { destroyed.push_back(event); });

    REQUIRE(runRuntimeTask(runtime, runtime.library().jobs().applyLibraryImportPlanAsync(std::move(*planRes))));
    settleRuntimeCallbacks(runtime);

    // The restored lists carry new ids, so the old views name no list; All Tracks survives any restore
    // and, as the last remaining view, takes focus from a closed active view. Every stale view closes
    // in the one commit.
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().cause == WorkspaceChangeCause::ListDeletion);
    CHECK(changes.front().snapshot.openViews == std::vector{allTracksViewId});
    CHECK(changes.front().snapshot.activeViewId == allTracksViewId);
    CHECK(changes.front().snapshot.revision == before.revision + 1);
    CHECK(runtime.workspace().snapshot() == changes.front().snapshot);
    REQUIRE(destroyed.size() == closedViewIds.size());

    for (auto const viewId : closedViewIds)
    {
      CAPTURE(viewId);
      CHECK(std::ranges::contains(destroyed, viewId, &ViewService::ViewDestroyed::viewId));
      auto const staleRes = runtime.views().findTrackListState(viewId);
      REQUIRE_FALSE(staleRes);
      CHECK(staleRes.error().code == Error::Code::NotFound);
    }
  }

  TEST_CASE("AppRuntime - jumpToAlbum reveals valid tracks in album presentation")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const trackId =
      TrackId{100}; // jumpToAlbum doesn't validate if track exists in library, it just passes the ID to playback
    auto const* songsPreset = builtinTrackPresentationPreset("songs");
    REQUIRE(songsPreset != nullptr);
    auto const existingViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = GlobalViewKind::AllTracks,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::Override,
          .spec = songsPreset->spec,
        },
    }));

    auto revealRequests = std::vector<PlaybackRevealTrackRequest>{};
    auto const sub = runtime.playback().events().onRevealTrackRequested(
      [&](PlaybackRevealTrackRequest const& request) noexcept { revealRequests.push_back(request); });

    auto const res = runtime.jumpToAlbum(trackId);
    REQUIRE(res);
    settleRuntimeCallbacks(runtime);
    REQUIRE(revealRequests.size() == 1);
    CHECK(revealRequests[0].trackId == trackId);
    CHECK(revealRequests[0].preferredViewId == existingViewId);
    CHECK(revealRequests[0].preferredListId == kInvalidListId);

    auto state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(runtime.workspace().snapshot().activeViewId == existingViewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(state.presentation.id == "albums");
  }

  TEST_CASE("WorkspaceService - invalid navigation targets return an error")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const res = runtime.workspace().navigate({.target = static_cast<GlobalViewKind>(999)});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(runtime.workspace().snapshot().activeViewId == kInvalidViewId);
  }

  TEST_CASE("WorkspaceService - empty navigation request rejects its invalid default target")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const res = runtime.workspace().navigate({});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(runtime.workspace().snapshot().activeViewId == kInvalidViewId);
    CHECK(runtime.workspace().snapshot().openViews.empty());
  }

  TEST_CASE("WorkspaceService - missing-list navigation leaves views focus and history unchanged")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));
    requireNavigation(runtime, fixture.secondListId);
    requireBackNavigation(runtime);
    auto const beforeLayout = runtime.workspace().snapshot();

    auto const res = runtime.workspace().navigate({.target = ListId{999999}});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
    auto const afterLayout = runtime.workspace().snapshot();
    CHECK(afterLayout.activeViewId == beforeLayout.activeViewId);
    CHECK(afterLayout.openViews == beforeLayout.openViews);
    CHECK(afterLayout.revision == beforeLayout.revision);
    CHECK(afterLayout.openViews.size() == beforeLayout.openViews.size());
    CHECK_FALSE(runtime.workspace().canGoBack());
    REQUIRE(runtime.workspace().canGoForward());
    requireForwardNavigation(runtime);
    CHECK(runtime.views().trackListState(runtime.workspace().snapshot().activeViewId).listId == fixture.secondListId);
    requireBackNavigation(runtime);
    CHECK(runtime.workspace().snapshot().activeViewId == beforeLayout.activeViewId);
  }

  TEST_CASE("WorkspaceService - focus rejects ids outside the open live aggregate")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const activeViewId = requireNavigation(runtime, fixture.firstListId);
    auto const before = runtime.workspace().snapshot();

    for (auto const viewId : {kInvalidViewId, ViewId{999999}})
    {
      auto const res = runtime.workspace().focusView(viewId);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
    }

    CHECK(runtime.workspace().snapshot() == before);
    CHECK(runtime.workspace().snapshot().activeViewId == activeViewId);
  }

  TEST_CASE("WorkspaceService - no-op focus and close preserve revision and observation")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& workspace = fixture.runtime().workspace();
    auto const activeViewId = requireNavigation(fixture.runtime(), fixture.firstListId);
    auto const before = workspace.snapshot();
    std::int32_t changeCount = 0;
    auto const sub = workspace.onChanged([&](WorkspaceChanged const&) noexcept { ++changeCount; });

    REQUIRE(workspace.focusView(activeViewId));
    REQUIRE(workspace.closeView(ViewId{999999}));

    CHECK(workspace.snapshot() == before);
    settleRuntimeCallbacks(fixture.runtime());
    CHECK(changeCount == 0);
  }

  TEST_CASE("WorkspaceService - focus commits a different open live view once")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& workspace = fixture.runtime().workspace();
    auto const firstViewId = requireNavigation(fixture.runtime(), fixture.firstListId);
    requireNavigation(fixture.runtime(), fixture.secondListId);
    auto const before = workspace.snapshot();
    auto changed = WorkspaceChanged{};
    auto const sub = workspace.onChanged([&](WorkspaceChanged const& value) noexcept { changed = value; });

    REQUIRE(workspace.focusView(firstViewId));
    settleRuntimeCallbacks(fixture.runtime());

    CHECK(workspace.snapshot().revision == before.revision + 1);
    CHECK(workspace.snapshot().activeViewId == firstViewId);
    CHECK(changed.cause == WorkspaceChangeCause::Focus);
    CHECK(changed.snapshot == workspace.snapshot());
  }

  TEST_CASE("WorkspaceService - changed observations are deferred to contract-fulfilling observers")
  {
    // The owning Signal emission boundary accepts ordinary handlers so it can
    // diagnose an escaping exception before aborting. Contract-fulfilling
    // observers receive the same committed snapshot in connection order.
    STATIC_REQUIRE_FALSE(
      std::is_nothrow_invocable_v<async::Signal<WorkspaceChanged const&>::Handler, WorkspaceChanged const&>);
    STATIC_REQUIRE(std::is_constructible_v<async::Signal<WorkspaceChanged const&>::Handler,
                                           decltype([](WorkspaceChanged const&) {})>);

    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr));
    auto received = std::vector<WorkspaceChanged>{};
    bool leadingObserverEntered = false;
    bool receivedAfterLeadingObserver = false;
    auto const leadingSub =
      runtimePtr->workspace().onChanged([&](WorkspaceChanged const&) noexcept { leadingObserverEntered = true; });
    auto const receivingSub = runtimePtr->workspace().onChanged(
      [&](WorkspaceChanged const& changed) noexcept
      {
        receivedAfterLeadingObserver = leadingObserverEntered;
        received.push_back(changed);
      });

    REQUIRE(runtimePtr->workspace().navigate({.target = GlobalViewKind::AllTracks}));

    CHECK(received.empty());
    CHECK_NOTHROW(executor->drain());
    CHECK(leadingObserverEntered);
    CHECK(receivedAfterLeadingObserver);
    REQUIRE(received.size() == 1);
    CHECK(received.front().snapshot == runtimePtr->workspace().snapshot());
  }

  TEST_CASE("WorkspaceService - reentrant changes cannot mutate the observation being delivered")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr));
    auto const firstListId = ao::test::requireValue(runRuntimeTask(
      *runtimePtr, runtimePtr->library().commands().createListAsync(ListDraft{.name = "First observed"})));
    executor->drain();
    auto const secondListId = ao::test::requireValue(runRuntimeTask(
      *runtimePtr, runtimePtr->library().commands().createListAsync(ListDraft{.name = "Second observed"})));
    executor->drain();
    auto received = std::vector<WorkspaceChanged>{};
    bool reentrantNavigateSucceeded = false;
    auto observationAfterReentry = WorkspaceSnapshot{};
    auto const sub = runtimePtr->workspace().onChanged(
      [&](WorkspaceChanged const& changed) noexcept
      {
        received.push_back(changed);

        if (received.size() == 1)
        {
          reentrantNavigateSucceeded = static_cast<bool>(runtimePtr->workspace().navigate({.target = secondListId}));
          observationAfterReentry = changed.snapshot;
        }
      });

    REQUIRE(runtimePtr->workspace().navigate({.target = firstListId}));
    auto const firstSnapshot = runtimePtr->workspace().snapshot();
    auto const firstRevision = firstSnapshot.revision;
    REQUIRE(executor->tryDrainUntil([&] { return received.size() == 1; }));

    REQUIRE(received.size() == 1);
    CHECK(reentrantNavigateSucceeded);
    auto const firstObservation = received.front();
    CHECK(firstObservation.snapshot == firstSnapshot);
    CHECK(observationAfterReentry == firstSnapshot);
    CHECK(firstObservation.snapshot.revision == firstRevision);
    CHECK(runtimePtr->workspace().snapshot().revision == firstRevision + 1);
    CHECK(received.front() == firstObservation);

    REQUIRE(executor->tryDrainUntil([&] { return received.size() == 2; }));
    CHECK(received.back().snapshot == runtimePtr->workspace().snapshot());
  }
} // namespace ao::rt::test
