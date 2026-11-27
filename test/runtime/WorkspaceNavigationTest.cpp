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
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::test;

  namespace
  {
    constexpr auto kMatchingTrackFilter = R"($title = "Shared Song")";

    struct TrackSourceNavigationIds final
    {
      ListId sourceListId;
      ListId otherListId;
      TrackId trackId;
    };

    class TrackSourceNavigationFixture final
    {
    public:
      AppRuntime& runtime() const noexcept { return _ownerPtr->runtime(); }
      LibraryCommands& commands() const noexcept { return _ownerPtr->libraryCommands(); }
      TrackSourceNavigationIds ids() const noexcept { return _ids; }

    private:
      ListId createList(std::string name, std::string expression) const
      {
        auto draft = ListDraft{.name = std::move(name), .expression = std::move(expression)};
        return requireValue(runRuntimeTask(runtime(), commands().createListAsync(std::move(draft))));
      }

      TempDir _tempDir;
      std::unique_ptr<AppRuntimeOwner> _ownerPtr = makeRuntime(_tempDir, std::make_unique<async::LoopExecutor>());
      TrackSourceNavigationIds _ids = {
        .sourceListId = createList("Source A", "#source"),
        .otherListId = createList("Other B", "#other"),
        .trackId = addRuntimeTrack(*_ownerPtr, {.title = "Shared Song", .tags = {"source", "other"}}),
      };
    };

    TrackPresentationSpec presetSpec(BuiltinPreset id)
    {
      return normalizeTrackPresentationSpec(builtinTrackPresentationPreset(id).spec);
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

    REQUIRE(runtime.workspace().navigate({.target = fixture.listIds().first}));

    auto const layout = runtime.workspace().snapshot();
    CHECK(layout.activeViewId != kInvalidViewId);
    auto const state = runtime.views().trackListState(layout.activeViewId);
    CHECK(state.listId == fixture.listIds().first);
  }

  TEST_CASE("WorkspaceService - navigate AllTracks opens the global list")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    REQUIRE(runtime.workspace().navigate({.target = fixture.listIds().first}));
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
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const ids = fixture.ids();
    auto const existingPresentation = presetSpec(BuiltinPreset::Songs);
    auto const reopenPresentation = presetSpec(BuiltinPreset::Albums);
    auto const previousPresentation = presetSpec(BuiltinPreset::Artists);
    auto const sourceViewId =
      requireNavigation(runtime,
                        NavigationRequest{.target = ids.sourceListId,
                                          .optPresentation = NavigationPresentation{.spec = existingPresentation}});
    REQUIRE(hasVisibleTrack(runtime, sourceViewId, ids.trackId));
    auto const previousViewId = requireNavigation(
      runtime,
      NavigationRequest{
        .target = FilteredListTarget{.listId = ids.otherListId, .filterExpression = kMatchingTrackFilter},
        .recordHistory = false,
        .optPresentation = NavigationPresentation{.spec = previousPresentation},
      });
    REQUIRE(hasVisibleTrack(runtime, previousViewId, ids.trackId));

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
      requireValue(workspace.navigateToTrackSource(ids.trackId, ids.sourceListId, defaultPresentation));

    CHECK(presentationRequests == (sourceClosed ? std::vector{ids.sourceListId} : std::vector<ListId>{}));
    CHECK(workspace.snapshot().activeViewId == viewId);
    CHECK((viewId != sourceViewId) == sourceClosed);
    auto const state = runtime.views().trackListState(viewId);
    CHECK(state.listId == ids.sourceListId);
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation == (sourceClosed ? reopenPresentation : existingPresentation));
    CHECK(state.selection.empty());
    CHECK(hasVisibleTrack(runtime, viewId, ids.trackId));

    // The active B view was not recorded when opened, so Back needs a new checkpoint for it.
    auto const backViewId = requireBackNavigation(runtime);
    CHECK(backViewId == previousViewId);
    auto const backState = runtime.views().trackListState(backViewId);
    CHECK(backState.listId == ids.otherListId);
    CHECK(backState.filterExpression == kMatchingTrackFilter);
    CHECK(backState.presentation == previousPresentation);
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource falls back to All Tracks when the source loses the track")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const& library = runtime.library();
    auto const ids = fixture.ids();
    auto const sourceViewId = requireNavigation(runtime, ids.sourceListId);
    auto const otherViewId = requireNavigation(runtime, ids.otherListId);
    REQUIRE(hasVisibleTrack(runtime, sourceViewId, ids.trackId));

    SECTION("The source list was deleted")
    {
      REQUIRE(runRuntimeTask(runtime, fixture.commands().deleteListAsync(ids.sourceListId)));
    }

    SECTION("The track was removed from the source list")
    {
      auto targets = requireValue(library.bindTrackTargets(std::array{ids.trackId}));
      auto const removedRes =
        runRuntimeTask(runtime, fixture.commands().removeTracksFromListAsync(ids.sourceListId, std::move(targets)));
      REQUIRE(removedRes);
      settleRuntimeCallbacks(runtime);
      REQUIRE_FALSE(hasVisibleTrack(runtime, sourceViewId, ids.trackId));
    }

    REQUIRE(hasVisibleTrack(runtime, otherViewId, ids.trackId));
    auto const allTracksPresentation = presetSpec(BuiltinPreset::Albums);
    auto const defaultPresentation = [&](ListId listId)
    { return listId == kAllTracksListId ? allTracksPresentation : defaultTrackPresentationSpec(); };

    auto const viewId =
      requireValue(workspace.navigateToTrackSource(ids.trackId, ids.sourceListId, defaultPresentation));

    CHECK(workspace.snapshot().activeViewId == viewId);
    auto const state = runtime.views().trackListState(viewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation == allTracksPresentation);
    CHECK(hasVisibleTrack(runtime, viewId, ids.trackId));
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource opens All Tracks for an unspecified source")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const ids = fixture.ids();
    auto const departureViewId = requireNavigation(runtime, ids.otherListId);
    REQUIRE(hasVisibleTrack(runtime, departureViewId, ids.trackId));
    auto const before = workspace.snapshot();

    auto const viewId = requireValue(workspace.navigateToTrackSource(ids.trackId, kInvalidListId));

    auto const after = workspace.snapshot();
    CHECK(after.activeViewId == viewId);
    CHECK(after.openViews == std::vector{departureViewId, viewId});
    CHECK(after.revision == before.revision + 1);
    auto const state = runtime.views().trackListState(viewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(state.filterExpression.empty());
    CHECK(hasVisibleTrack(runtime, viewId, ids.trackId));
    CHECK_FALSE(workspace.canGoForward());
    REQUIRE(requireBackNavigation(runtime) == departureViewId);
    CHECK_FALSE(workspace.canGoBack());
    CHECK(requireForwardNavigation(runtime) == viewId);
  }

  TEST_CASE(
    "WorkspaceService - navigateToTrackSource reuses inactive plain All Tracks for an explicit All Tracks source")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const ids = fixture.ids();
    requireNavigation(
      runtime, FilteredListTarget{.listId = kAllTracksListId, .filterExpression = kMatchingTrackFilter});
    auto const presentation = presetSpec(BuiltinPreset::Songs);
    auto const allTracksViewId =
      requireNavigation(runtime,
                        NavigationRequest{.target = GlobalViewKind::AllTracks,
                                          .optPresentation = NavigationPresentation{.spec = presentation}});
    auto const departureViewId = requireNavigation(runtime, ids.otherListId);
    REQUIRE(hasVisibleTrack(runtime, departureViewId, ids.trackId));
    auto const before = workspace.snapshot();
    bool requestedPresentation = false;
    auto const defaultPresentation = [&](ListId)
    {
      requestedPresentation = true;
      return defaultTrackPresentationSpec();
    };

    auto const viewId =
      requireValue(workspace.navigateToTrackSource(ids.trackId, kAllTracksListId, defaultPresentation));

    CHECK(viewId == allTracksViewId);
    auto const after = workspace.snapshot();
    CHECK(after.activeViewId == allTracksViewId);
    CHECK(after.openViews == before.openViews);
    CHECK(after.revision == before.revision + 1);
    CHECK_FALSE(requestedPresentation);
    CHECK(runtime.views().trackListState(allTracksViewId).presentation == presentation);
    CHECK(hasVisibleTrack(runtime, allTracksViewId, ids.trackId));
    CHECK_FALSE(workspace.canGoForward());
    REQUIRE(requireBackNavigation(runtime) == departureViewId);
    CHECK(requireForwardNavigation(runtime) == allTracksViewId);
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource opens a plain view when the source filter hides the track")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const ids = fixture.ids();
    auto const filteredViewId = requireNavigation(runtime, ids.sourceListId);
    REQUIRE(hasVisibleTrack(runtime, filteredViewId, ids.trackId));
    auto const previousPresentation = presetSpec(BuiltinPreset::Artists);
    REQUIRE(workspace.setActivePresentation(previousPresentation, {.recordHistory = false}));
    auto const* const filter = R"($title = "Different Song")";
    REQUIRE(runtime.views().setFilter(filteredViewId, filter));
    settleRuntimeCallbacks(runtime);
    REQUIRE_FALSE(hasVisibleTrack(runtime, filteredViewId, ids.trackId));

    auto const viewId = requireValue(workspace.navigateToTrackSource(ids.trackId, ids.sourceListId));

    CHECK(workspace.snapshot().activeViewId == viewId);
    CHECK(viewId != filteredViewId);
    auto const state = runtime.views().trackListState(viewId);
    CHECK(state.listId == ids.sourceListId);
    CHECK(state.filterExpression.empty());
    CHECK(hasVisibleTrack(runtime, viewId, ids.trackId));
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
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto& views = runtime.views();
    auto const ids = fixture.ids();
    auto const filteredViewId = requireNavigation(
      runtime, FilteredListTarget{.listId = ids.sourceListId, .filterExpression = kMatchingTrackFilter});
    REQUIRE(views.setSelection(filteredViewId, {ids.trackId}));
    auto const departureViewId = requireNavigation(runtime, ids.otherListId);
    auto const departurePresentation = presetSpec(BuiltinPreset::Artists);
    REQUIRE(views.setFilter(departureViewId, kMatchingTrackFilter));
    REQUIRE(workspace.setActivePresentation(departurePresentation, {.recordHistory = false}));
    settleRuntimeCallbacks(runtime);

    REQUIRE(workspace.snapshot().openViews == std::vector{filteredViewId, departureViewId});
    auto const filteredBefore = views.trackListState(filteredViewId);
    REQUIRE(filteredBefore.filterExpression == kMatchingTrackFilter);
    REQUIRE(hasVisibleTrack(runtime, filteredViewId, ids.trackId));

    auto const viewId = requireValue(workspace.navigateToTrackSource(ids.trackId, ids.sourceListId));

    auto const after = workspace.snapshot();
    CHECK(after.activeViewId == viewId);
    CHECK(after.openViews == std::vector{filteredViewId, departureViewId, viewId});
    auto const state = requireValue(views.findTrackListState(viewId));
    CHECK(state.listId == ids.sourceListId);
    CHECK(state.filterExpression.empty());
    CHECK(hasVisibleTrack(runtime, viewId, ids.trackId));
    auto const filteredAfter = requireValue(views.findTrackListState(filteredViewId));
    CHECK(filteredAfter.filterExpression == kMatchingTrackFilter);
    CHECK(filteredAfter.presentation == filteredBefore.presentation);
    CHECK(filteredAfter.selection == std::vector{ids.trackId});
    CHECK(hasVisibleTrack(runtime, filteredViewId, ids.trackId));

    // The departing filter and presentation changed after the last history entry.
    REQUIRE(requireBackNavigation(runtime) == departureViewId);
    auto const backState = views.trackListState(departureViewId);
    CHECK(backState.filterExpression == kMatchingTrackFilter);
    CHECK(backState.presentation == departurePresentation);
  }

  TEST_CASE("WorkspaceService - navigateToTrackSource keeps the active filtered source if it shows the track")
  {
    auto fixture = TrackSourceNavigationFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const ids = fixture.ids();
    auto const viewId = requireNavigation(
      runtime, FilteredListTarget{.listId = ids.sourceListId, .filterExpression = kMatchingTrackFilter});
    auto const forwardViewId = requireNavigation(runtime, ids.otherListId);
    REQUIRE(requireBackNavigation(runtime) == viewId);
    REQUIRE(hasVisibleTrack(runtime, viewId, ids.trackId));
    auto const presentation = presetSpec(BuiltinPreset::Artists);
    REQUIRE(workspace.setActivePresentation(presentation, {.recordHistory = false}));
    auto const before = workspace.snapshot();
    bool requestedPresentation = false;
    auto const defaultPresentation = [&](ListId)
    {
      requestedPresentation = true;
      return defaultTrackPresentationSpec();
    };

    auto const resolvedViewId =
      requireValue(workspace.navigateToTrackSource(ids.trackId, ids.sourceListId, defaultPresentation));

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
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto& views = runtime.views();
    auto const ids = fixture.ids();
    auto const viewId = requireNavigation(
      runtime, FilteredListTarget{.listId = ids.otherListId, .filterExpression = kMatchingTrackFilter});
    auto const forwardViewId = requireNavigation(runtime, ids.sourceListId);
    REQUIRE(requireBackNavigation(runtime) == viewId);
    REQUIRE(views.setSelection(viewId, {ids.trackId}));
    auto const before = workspace.snapshot();
    auto const beforeProjectionPtr = requireValue(views.findTrackListProjection(viewId));
    auto const beforeRevision = runtime.library().snapshot().revision();
    auto const missingTrackId = TrackId{999999};
    REQUIRE_FALSE(runtime.library().snapshot().containsTrack(missingTrackId));
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub = workspace.onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });

    auto const res = workspace.navigateToTrackSource(missingTrackId, ids.sourceListId);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
    settleRuntimeCallbacks(runtime);
    CHECK(workspace.snapshot() == before);
    CHECK(changes.empty());
    CHECK(views.trackListState(viewId).selection == std::vector{ids.trackId});
    CHECK(requireValue(views.findTrackListProjection(viewId)) == beforeProjectionPtr);
    CHECK(requireValue(views.listSourceTrackIds(forwardViewId)) == std::vector{ids.trackId});
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

    REQUIRE(runtime.workspace().navigate({.target = fixture.listIds().first}));
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
    CHECK(state.listId == fixture.listIds().first);
  }

  TEST_CASE("WorkspaceService - onChanged includes the committed focus")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto focusedViewId = kInvalidViewId;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept
                                                   { focusedViewId = changed.snapshot.activeViewId; });

    REQUIRE(runtime.workspace().navigate({.target = fixture.listIds().first}));
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

    REQUIRE(runRuntimeTask(runtime, fixture.commands().deleteListAsync(listId)));

    auto layout = runtime.workspace().snapshot();
    CHECK(!std::ranges::contains(layout.openViews, activeViewId));
  }

  TEST_CASE("WorkspaceService - deleting a list closes all matching views in one commit")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    requireNavigation(
      runtime, FilteredListTarget{.listId = fixture.listIds().first, .filterExpression = "$title ~ \"A\""});
    requireNavigation(
      runtime, FilteredListTarget{.listId = fixture.listIds().first, .filterExpression = "$title ~ \"B\""});
    auto const before = runtime.workspace().snapshot();
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub =
      runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });

    REQUIRE(runRuntimeTask(runtime, fixture.commands().deleteListAsync(fixture.listIds().first)));

    REQUIRE(changes.size() == 1);
    CHECK(changes.front().cause == WorkspaceChangeCause::ListDeletion);
    CHECK(changes.front().snapshot.openViews.empty());
    CHECK(changes.front().snapshot.activeViewId == kInvalidViewId);
    CHECK(changes.front().snapshot.revision == before.revision + 1);
  }

  TEST_CASE("WorkspaceService - filtered navigation checkpoints the view recovered after list deletion")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const allTracks = requireNavigation(runtime, GlobalViewKind::AllTracks);
    auto const deleted = requireNavigation(runtime, fixture.listIds().first);
    REQUIRE(runRuntimeTask(runtime, fixture.commands().deleteListAsync(fixture.listIds().first)));
    settleRuntimeCallbacks(runtime);
    auto const recovered = workspace.snapshot();
    REQUIRE(recovered.activeViewId == allTracks);
    REQUIRE(recovered.openViews == std::vector{allTracks});
    REQUIRE_FALSE(runtime.views().findTrackListState(deleted));
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub = workspace.onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });
    auto const* const artistFilter = R"($artist = "Example Artist")";

    SECTION("A failed destination does not record recovery")
    {
      auto const res = workspace.navigate(
        {.target = FilteredListTarget{.listId = fixture.listIds().first, .filterExpression = artistFilter}});

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
      settleRuntimeCallbacks(runtime);
      CHECK(workspace.snapshot() == recovered);
      CHECK(changes.empty());
      CHECK_FALSE(workspace.canGoForward());
      REQUIRE(requireBackNavigation(runtime) == allTracks);
      CHECK_FALSE(workspace.canGoBack());
      REQUIRE(workspace.canGoForward());
      auto const beforeReplay = workspace.snapshot();
      auto const forwardRes = workspace.goForward();
      REQUIRE_FALSE(forwardRes);
      CHECK(forwardRes.error().code == Error::Code::NotFound);
      CHECK(workspace.snapshot() == beforeReplay);
    }

    SECTION("A successful destination records recovery and the requested target in one commit")
    {
      auto const artistView =
        requireNavigation(runtime, FilteredListTarget{.listId = kAllTracksListId, .filterExpression = artistFilter});

      auto const navigated = workspace.snapshot();
      CHECK(navigated.activeViewId == artistView);
      CHECK(navigated.openViews == std::vector{allTracks, artistView});
      CHECK(navigated.revision == recovered.revision + 1);
      auto const artistState = runtime.views().trackListState(artistView);
      CHECK(artistState.listId == kAllTracksListId);
      CHECK(artistState.filterExpression == artistFilter);
      REQUIRE(changes.size() == 1);
      CHECK(changes.front().cause == WorkspaceChangeCause::Navigation);
      CHECK(changes.front().snapshot == navigated);

      REQUIRE(requireBackNavigation(runtime) == allTracks);
      CHECK(runtime.views().trackListState(allTracks).filterExpression.empty());
      auto const beforeReplay = workspace.snapshot();
      changes.clear();
      auto const backRes = workspace.goBack();
      REQUIRE_FALSE(backRes);
      CHECK(backRes.error().code == Error::Code::NotFound);
      settleRuntimeCallbacks(runtime);
      CHECK(workspace.snapshot() == beforeReplay);
      CHECK(changes.empty());
      REQUIRE(workspace.canGoForward());
      CHECK(requireForwardNavigation(runtime) == artistView);
      CHECK_FALSE(workspace.canGoForward());
    }
  }

  TEST_CASE("WorkspaceService - a library restore closes the views of lists it replaced")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto closedViewIds = std::vector<ViewId>{};
    auto allTracksViewId = kInvalidViewId;

    SECTION("All Tracks is focused")
    {
      closedViewIds.push_back(requireNavigation(runtime, fixture.listIds().first));
      allTracksViewId = requireNavigation(runtime, GlobalViewKind::AllTracks);
    }

    SECTION("The replaced list's view is focused")
    {
      allTracksViewId = requireNavigation(runtime, GlobalViewKind::AllTracks);
      closedViewIds.push_back(requireNavigation(runtime, fixture.listIds().first));
    }

    SECTION("Several filtered views of a replaced list are open")
    {
      allTracksViewId = requireNavigation(runtime, GlobalViewKind::AllTracks);
      closedViewIds.push_back(requireNavigation(
        runtime, FilteredListTarget{.listId = fixture.listIds().first, .filterExpression = "$title ~ \"A\""}));
      closedViewIds.push_back(requireNavigation(
        runtime, FilteredListTarget{.listId = fixture.listIds().first, .filterExpression = "$title ~ \"B\""}));
    }

    auto const before = runtime.workspace().snapshot();
    auto const yamlPath = fixture.tempPath() / "backup.yaml";
    REQUIRE(runRuntimeTask(runtime, fixture.jobs().exportLibraryAsync(yamlPath, ExportMode::Full)));
    auto planRes = runRuntimeTask(runtime, fixture.jobs().prepareLibraryImportAsync(yamlPath, ImportMode::Restore));
    REQUIRE(planRes);
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub =
      runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });
    auto destroyed = std::vector<ViewService::ViewDestroyed>{};
    auto const destroyedSub = runtime.views().onViewDestroyed([&](ViewService::ViewDestroyed const& event) noexcept
                                                              { destroyed.push_back(event); });

    REQUIRE(runRuntimeTask(runtime, fixture.jobs().applyLibraryImportPlanAsync(std::move(*planRes))));
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
    auto const existingViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = GlobalViewKind::AllTracks,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::Override,
          .spec = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec,
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
    CHECK(state.presentation.isPreset(BuiltinPreset::Albums));
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
    REQUIRE(runtime.workspace().navigate({.target = fixture.listIds().first}));
    requireNavigation(runtime, fixture.listIds().second);
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
    CHECK(runtime.views().trackListState(runtime.workspace().snapshot().activeViewId).listId ==
          fixture.listIds().second);
    requireBackNavigation(runtime);
    CHECK(runtime.workspace().snapshot().activeViewId == beforeLayout.activeViewId);
  }

  TEST_CASE("WorkspaceService - navigating to an open view commits its destination and edited departure together")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const first = requireNavigation(runtime, fixture.listIds().first);
    auto const second = requireNavigation(runtime, fixture.listIds().second);
    REQUIRE(requireBackNavigation(runtime) == first);
    REQUIRE(runtime.views().setFilter(first, kMatchingTrackFilter));
    settleRuntimeCallbacks(runtime);
    auto const before = workspace.snapshot();
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub = workspace.onChanged([&](WorkspaceChanged const& change) noexcept { changes.push_back(change); });

    auto const missingRes = workspace.navigateToOpenView(ViewId{999999});
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::InvalidInput);
    CHECK(requireValue(workspace.navigateToOpenView(first)) == first);
    settleRuntimeCallbacks(runtime);
    CHECK(workspace.snapshot() == before);
    CHECK(changes.empty());
    CHECK_FALSE(workspace.canGoBack());
    REQUIRE(workspace.canGoForward());

    REQUIRE(requireValue(workspace.navigateToOpenView(second)) == second);
    settleRuntimeCallbacks(runtime);
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().cause == WorkspaceChangeCause::Navigation);
    CHECK(changes.front().snapshot.activeViewId == second);
    CHECK(changes.front().snapshot.revision == before.revision + 1);
    REQUIRE(requireBackNavigation(runtime) == first);
    CHECK(runtime.views().trackListState(first).filterExpression == kMatchingTrackFilter);
  }

  TEST_CASE("WorkspaceService - focus rejects ids outside the open live aggregate")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const activeViewId = requireNavigation(runtime, fixture.listIds().first);
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
    auto const activeViewId = requireNavigation(fixture.runtime(), fixture.listIds().first);
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
    auto const firstViewId = requireNavigation(fixture.runtime(), fixture.listIds().first);
    requireNavigation(fixture.runtime(), fixture.listIds().second);
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

  TEST_CASE("WorkspaceService - recorded navigation does not append a deliberately focused departure")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto const first = requireNavigation(runtime, fixture.listIds().first);
    auto const second = requireNavigation(runtime, fixture.listIds().second);
    REQUIRE(workspace.focusView(first));
    CHECK(workspace.snapshot().activeViewId == first);

    requireNavigation(runtime, fixture.listIds().third);

    REQUIRE(requireBackNavigation(runtime) == second);
    CHECK(requireBackNavigation(runtime) == first);
    CHECK_FALSE(workspace.canGoBack());
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
    auto ownerPtr = makeRuntime(tempDir, std::move(executorPtr));
    auto& runtime = ownerPtr->runtime();
    auto received = std::vector<WorkspaceChanged>{};
    bool leadingObserverEntered = false;
    bool receivedAfterLeadingObserver = false;
    auto const leadingSub =
      runtime.workspace().onChanged([&](WorkspaceChanged const&) noexcept { leadingObserverEntered = true; });
    auto const receivingSub = runtime.workspace().onChanged(
      [&](WorkspaceChanged const& changed) noexcept
      {
        receivedAfterLeadingObserver = leadingObserverEntered;
        received.push_back(changed);
      });

    REQUIRE(runtime.workspace().navigate({.target = GlobalViewKind::AllTracks}));

    CHECK(received.empty());
    CHECK_NOTHROW(executor->drain());
    CHECK(leadingObserverEntered);
    CHECK(receivedAfterLeadingObserver);
    REQUIRE(received.size() == 1);
    CHECK(received.front().snapshot == runtime.workspace().snapshot());
  }

  TEST_CASE("WorkspaceService - reentrant changes cannot mutate the observation being delivered")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto ownerPtr = makeRuntime(tempDir, std::move(executorPtr));
    auto& runtime = ownerPtr->runtime();
    auto const firstListId = ao::test::requireValue(
      runRuntimeTask(runtime, ownerPtr->libraryCommands().createListAsync(ListDraft{.name = "First observed"})));
    executor->drain();
    auto const secondListId = ao::test::requireValue(
      runRuntimeTask(runtime, ownerPtr->libraryCommands().createListAsync(ListDraft{.name = "Second observed"})));
    executor->drain();
    auto received = std::vector<WorkspaceChanged>{};
    bool reentrantNavigateSucceeded = false;
    auto observationAfterReentry = WorkspaceSnapshot{};
    auto const sub = runtime.workspace().onChanged(
      [&](WorkspaceChanged const& changed) noexcept
      {
        received.push_back(changed);

        if (received.size() == 1)
        {
          reentrantNavigateSucceeded = static_cast<bool>(runtime.workspace().navigate({.target = secondListId}));
          observationAfterReentry = changed.snapshot;
        }
      });

    REQUIRE(runtime.workspace().navigate({.target = firstListId}));
    auto const firstSnapshot = runtime.workspace().snapshot();
    auto const firstRevision = firstSnapshot.revision;
    REQUIRE(executor->tryDrainUntil([&] { return received.size() == 1; }));

    REQUIRE(received.size() == 1);
    CHECK(reentrantNavigateSucceeded);
    auto const firstObservation = received.front();
    CHECK(firstObservation.snapshot == firstSnapshot);
    CHECK(observationAfterReentry == firstSnapshot);
    CHECK(firstObservation.snapshot.revision == firstRevision);
    CHECK(runtime.workspace().snapshot().revision == firstRevision + 1);
    CHECK(received.front() == firstObservation);

    REQUIRE(executor->tryDrainUntil([&] { return received.size() == 2; }));
    CHECK(received.back().snapshot == runtime.workspace().snapshot());
  }
} // namespace ao::rt::test
