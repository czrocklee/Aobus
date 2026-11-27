// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/AppConfigStore.h"
#include "app/GtkLayoutStateStore.h"
#include "app/MainWindow.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    rt::TrackPresentationSpec const& presentation(rt::BuiltinPreset const id)
    {
      return rt::builtinTrackPresentationPreset(id).spec;
    }

    std::shared_ptr<AppConfigStore> appConfigStore(ao::test::TempDir const& tempDir)
    {
      return std::make_shared<AppConfigStore>(tempDir.path() / "app-config.yaml");
    }

    void savePresentationPreference(rt::AppRuntime& runtime, ListId const listId, rt::BuiltinPreset const preset)
    {
      auto layoutState = uimodel::TrackColumnLayouts::Snapshot{};
      auto preferenceState = uimodel::ListPresentations::Snapshot{};
      preferenceState.emplace(listId, preset);
      auto store = GtkLayoutStateStore{
        rt::LibraryPaths{runtime.musicRoot()}.managedDataPath(), runtime.library().snapshot().libraryId()};
      store.save(layoutState, preferenceState);
    }

    MainWindow makeSessionWindow(rt::AppRuntimeOwner& owner, ao::test::TempDir const& tempDir)
    {
      return MainWindow{owner, appConfigStore(tempDir), nullptr, ao::test::englishMessageCatalog()};
    }

    ListId createList(rt::AppRuntimeOwner& owner, std::string name)
    {
      return ao::test::requireValue(runGtkTask(owner.runtime(),
                                               owner.libraryCommands().createListAsync(rt::ListDraft{
                                                 .name = std::move(name),
                                               })));
    }

    TrackId addPlayableTrack(rt::AppRuntimeOwner& owner, std::string_view const title)
    {
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      return addRuntimeTrack(owner, library::test::TrackSpec{.title = std::string{title}, .uri = fixturePath});
    }

    std::vector<rt::ViewId> viewsForList(rt::AppRuntime& runtime, ListId const listId)
    {
      auto result = std::vector<rt::ViewId>{};

      for (auto const viewId : runtime.workspace().snapshot().openViews)
      {
        if (runtime.views().trackListState(viewId).listId == listId)
        {
          result.push_back(viewId);
        }
      }

      return result;
    }
  } // namespace

  TEST_CASE("MainWindow - restored workspace presentation survives startup and history replay")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto otherListId = kInvalidListId;

    {
      auto ownerPtr = makeRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      otherListId = createList(*ownerPtr, "History destination");
      REQUIRE(runtime.workspace().navigate({
        .target = rt::kAllTracksListId,
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = presentation(rt::BuiltinPreset::ListOrder),
          },
      }));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
      savePresentationPreference(runtime, rt::kAllTracksListId, rt::BuiltinPreset::Albums);
    }

    auto ownerPtr = makeRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto window = makeSessionWindow(*ownerPtr, tempDir);
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    REQUIRE(state.listId == rt::kAllTracksListId);
    CHECK(state.presentation.isPreset(rt::BuiltinPreset::ListOrder));

    REQUIRE(runtime.workspace().navigate({.target = otherListId}));
    REQUIRE(runtime.workspace().goBack());
    state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.presentation.isPreset(rt::BuiltinPreset::ListOrder));
  }

  TEST_CASE("MainWindow - empty workspace creates All Tracks with its saved preference")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};

    {
      auto ownerPtr = makeRuntime(tempDir);
      savePresentationPreference(ownerPtr->runtime(), rt::kAllTracksListId, rt::BuiltinPreset::Songs);
    }

    auto ownerPtr = makeRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto window = makeSessionWindow(*ownerPtr, tempDir);
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.presentation.isPreset(rt::BuiltinPreset::Songs));
  }

  TEST_CASE("MainWindow - ordinary list selection applies a default only to a new plain view")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;

    {
      auto ownerPtr = makeRuntime(tempDir);
      listId = createList(*ownerPtr, "Preferred list");
      savePresentationPreference(ownerPtr->runtime(), listId, rt::BuiltinPreset::Albums);
    }

    auto ownerPtr = makeRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto window = makeSessionWindow(*ownerPtr, tempDir);
    REQUIRE(window.prepareSession());

    // navigateToList is what the navigation tree drives when a list is picked.
    REQUIRE(window.navigateToList(listId));
    drainGtkEvents();
    auto const firstViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(firstViewId != rt::kInvalidViewId);
    CHECK(runtime.views().trackListState(firstViewId).presentation.isPreset(rt::BuiltinPreset::Albums));

    REQUIRE(runtime.workspace().setActivePresentation(presentation(rt::BuiltinPreset::Songs)));
    REQUIRE(window.navigateToList(rt::kAllTracksListId));
    drainGtkEvents();
    REQUIRE(window.navigateToList(listId));
    drainGtkEvents();

    CHECK(runtime.workspace().snapshot().activeViewId == firstViewId);
    CHECK(runtime.views().trackListState(firstViewId).presentation.isPreset(rt::BuiltinPreset::Songs));
  }

  TEST_CASE("MainWindow - playback restore reuses a restored plain view without changing presentation")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;
    auto trackId = kInvalidTrackId;

    {
      auto ownerPtr = makeRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      rt::test::addReadyAudioProvider(runtime);
      trackId = addPlayableTrack(*ownerPtr, "Restored plain track");
      listId = createList(*ownerPtr, "Restored plain list");
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = listId,
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = presentation(rt::BuiltinPreset::Songs),
          },
      }));
      REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
      REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));
      REQUIRE(runtime.savePlaybackSession());
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
      savePresentationPreference(runtime, listId, rt::BuiltinPreset::Albums);
      runtime.playback().commands().stop();
    }

    auto ownerPtr = makeRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto window = makeSessionWindow(*ownerPtr, tempDir);
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto const listViews = viewsForList(runtime, listId);
    REQUIRE(listViews.size() == 1);
    auto const state = runtime.views().trackListState(listViews.front());
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation.isPreset(rt::BuiltinPreset::Songs));
    CHECK(state.selection == std::vector<TrackId>{trackId});
  }

  TEST_CASE("MainWindow - replacement activation leaves stored playback identity idle and unrestored")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};

    {
      auto ownerPtr = makeRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      rt::test::addReadyAudioProvider(runtime);
      auto const trackId = addPlayableTrack(*ownerPtr, "Old library playback");
      runtime.sources().reloadAllTracks();
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
      REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));
      REQUIRE(runtime.savePlaybackSession());
      runtime.playback().commands().stop();
    }

    auto ownerPtr = makeRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto window = makeSessionWindow(*ownerPtr, tempDir);
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));

    auto const& snapshot = runtime.playback().snapshot();
    CHECK(snapshot.transport.transport == audio::Transport::Idle);
    CHECK(snapshot.transport.nowPlaying.trackId == kInvalidTrackId);
    CHECK(snapshot.succession.currentTrackId == kInvalidTrackId);
  }

  TEST_CASE("MainWindow - playback restore creates a preferred plain view beside a filtered view")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;

    {
      auto ownerPtr = makeRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      rt::test::addReadyAudioProvider(runtime);
      auto const trackId = addPlayableTrack(*ownerPtr, "Restored filtered track");
      listId = createList(*ownerPtr, "Restored filtered list");
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target =
          rt::FilteredListTarget{
            .listId = listId,
            .filterExpression = "$title ~ \"Restored\"",
          },
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = presentation(rt::BuiltinPreset::Songs),
          },
      }));
      drainGtkEvents();
      REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
      REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));
      REQUIRE(runtime.savePlaybackSession());
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
      savePresentationPreference(runtime, listId, rt::BuiltinPreset::Albums);
      runtime.playback().commands().stop();
    }

    auto ownerPtr = makeRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto window = makeSessionWindow(*ownerPtr, tempDir);
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto const listViews = viewsForList(runtime, listId);
    REQUIRE(listViews.size() == 2);
    std::int32_t filteredCount = 0;
    std::int32_t plainCount = 0;

    for (auto const viewId : listViews)
    {
      if (auto const state = runtime.views().trackListState(viewId); state.filterExpression.empty())
      {
        ++plainCount;
        CHECK(state.presentation.isPreset(rt::BuiltinPreset::Albums));
      }
      else
      {
        ++filteredCount;
        CHECK(state.filterExpression == "$title ~ \"Restored\"");
        CHECK(state.presentation.isPreset(rt::BuiltinPreset::Songs));
      }
    }

    CHECK(filteredCount == 1);
    CHECK(plainCount == 1);
  }

  TEST_CASE("MainWindow - playback restore creates a preferred plain view when no target view exists")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;
    auto trackId = kInvalidTrackId;

    {
      auto ownerPtr = makeRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      rt::test::addReadyAudioProvider(runtime);
      trackId = addPlayableTrack(*ownerPtr, "Restored new-view track");
      listId = createList(*ownerPtr, "Restored new-view list");
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
      REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
      REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));
      REQUIRE(runtime.savePlaybackSession());
      savePresentationPreference(runtime, listId, rt::BuiltinPreset::Albums);
      runtime.playback().commands().stop();
    }

    auto ownerPtr = makeRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto window = makeSessionWindow(*ownerPtr, tempDir);
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto const listViews = viewsForList(runtime, listId);
    REQUIRE(listViews.size() == 1);
    auto const state = runtime.views().trackListState(listViews.front());
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation.isPreset(rt::BuiltinPreset::Albums));
    CHECK(runtime.workspace().snapshot().activeViewId == listViews.front());
    CHECK(state.selection == std::vector<TrackId>{trackId});
  }
} // namespace ao::gtk::test
