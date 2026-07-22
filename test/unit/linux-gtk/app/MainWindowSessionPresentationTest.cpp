// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "../GtkTestSupport.h"
#include "app/AppConfigStore.h"
#include "app/GtkLayoutStateStore.h"
#include "app/MainWindow.h"
#include "app/MainWindowCoordinator.h"
#include "list/ListNavigationController.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

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
    rt::TrackPresentationSpec const& presentation(std::string_view const id)
    {
      auto const* const preset = rt::builtinTrackPresentationPreset(id);
      REQUIRE(preset != nullptr);
      return preset->spec;
    }

    std::shared_ptr<AppConfigStore> appConfigStore(ao::test::TempDir const& tempDir)
    {
      return std::make_shared<AppConfigStore>(tempDir.path() / "app-config.yaml");
    }

    void savePresentationPreference(ao::test::TempDir const& tempDir,
                                    ListId const listId,
                                    std::string const& presentationId)
    {
      auto layoutState = uimodel::TrackColumnLayoutState{};
      auto preferenceState = uimodel::ListPresentationPreferenceState{};
      preferenceState.presentations.emplace(listId, presentationId);
      auto store = GtkLayoutStateStore{rt::LibraryPaths{tempDir.path()}.managedDataPath()};
      store.save(layoutState, preferenceState);
    }

    ListId createManualList(rt::AppRuntime& runtime, std::string name, std::vector<TrackId> trackIds = {})
    {
      return ao::test::requireValue(runtime.library().writer().createList(rt::LibraryWriter::ListDraft{
        .kind = rt::LibraryWriter::ListKind::Manual,
        .name = std::move(name),
        .trackIds = std::move(trackIds),
      }));
    }

    TrackId addPlayableTrack(rt::AppRuntime& runtime, std::string_view const title)
    {
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      return addRuntimeTrack(runtime, {.title = std::string{title}, .uri = fixturePath});
    }

    std::vector<rt::ViewId> viewsForList(rt::AppRuntime& runtime, ListId const listId)
    {
      auto result = std::vector<rt::ViewId>{};

      for (auto const viewId : runtime.views().listViews())
      {
        if (runtime.views().trackListState(viewId).listId == listId)
        {
          result.push_back(viewId);
        }
      }

      return result;
    }
  } // namespace

  TEST_CASE("MainWindowCoordinator - restored workspace presentation survives startup and history replay",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto otherListId = kInvalidListId;

    {
      auto runtime = makeRuntime(tempDir);
      otherListId = createManualList(runtime, "History destination");
      REQUIRE(runtime.workspace().navigate({
        .target = rt::kAllTracksListId,
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = presentation(rt::kListOrderTrackPresentationId),
          },
      }));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
      savePresentationPreference(tempDir, rt::kAllTracksListId, "albums");
    }

    auto runtime = makeRuntime(tempDir);
    auto window = MainWindow{runtime, appConfigStore(tempDir), nullptr};
    window.initializeSession();
    drainGtkEvents();

    auto state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    REQUIRE(state.listId == rt::kAllTracksListId);
    CHECK(state.presentation.id == rt::kListOrderTrackPresentationId);

    REQUIRE(runtime.workspace().navigate({.target = otherListId}));
    REQUIRE(runtime.workspace().goBack());
    state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.presentation.id == rt::kListOrderTrackPresentationId);
  }

  TEST_CASE("MainWindowCoordinator - empty workspace creates All Tracks with its saved preference",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};

    {
      auto runtime = makeRuntime(tempDir);
      savePresentationPreference(tempDir, rt::kAllTracksListId, "songs");
    }

    auto runtime = makeRuntime(tempDir);
    auto window = MainWindow{runtime, appConfigStore(tempDir), nullptr};
    window.initializeSession();
    drainGtkEvents();

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.presentation.id == "songs");
  }

  TEST_CASE("MainWindowCoordinator - ordinary list selection applies a default only to a new plain view",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;

    {
      auto runtime = makeRuntime(tempDir);
      listId = createManualList(runtime, "Preferred manual list");
      savePresentationPreference(tempDir, listId, "albums");
    }

    auto runtime = makeRuntime(tempDir);
    auto configStorePtr = appConfigStore(tempDir);
    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};
    coordinator.loadSession();
    coordinator.initializeSession();

    coordinator.listNavigationController()->select(listId);
    drainGtkEvents();
    auto const firstViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(firstViewId != rt::kInvalidViewId);
    CHECK(runtime.views().trackListState(firstViewId).presentation.id == "albums");

    REQUIRE(runtime.workspace().setActivePresentation(presentation("songs")));
    coordinator.listNavigationController()->select(rt::kAllTracksListId);
    drainGtkEvents();
    coordinator.listNavigationController()->select(listId);
    drainGtkEvents();

    CHECK(runtime.workspace().snapshot().activeViewId == firstViewId);
    CHECK(runtime.views().trackListState(firstViewId).presentation.id == "songs");
  }

  TEST_CASE("MainWindowCoordinator - playback restore reuses a restored plain view without changing presentation",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;
    auto trackId = kInvalidTrackId;

    {
      auto runtime = makeRuntime(tempDir);
      rt::test::addReadyAudioProvider(runtime);
      trackId = addPlayableTrack(runtime, "Restored plain track");
      listId = createManualList(runtime, "Restored plain list", {trackId});
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = listId,
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = presentation("songs"),
          },
      }));
      REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
      REQUIRE(runtime.savePlaybackSession());
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
      savePresentationPreference(tempDir, listId, "albums");
      runtime.playback().commands().stop();
    }

    auto runtime = makeRuntime(tempDir);
    rt::test::addReadyAudioProvider(runtime);
    auto window = MainWindow{runtime, appConfigStore(tempDir), nullptr};
    window.initializeSession();
    drainGtkEvents();

    auto const listViews = viewsForList(runtime, listId);
    REQUIRE(listViews.size() == 1);
    auto const state = runtime.views().trackListState(listViews.front());
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation.id == "songs");
    CHECK(state.selection == std::vector<TrackId>{trackId});
  }

  TEST_CASE("MainWindowCoordinator - playback restore creates a preferred plain view beside a filtered view",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;

    {
      auto runtime = makeRuntime(tempDir);
      rt::test::addReadyAudioProvider(runtime);
      auto const trackId = addPlayableTrack(runtime, "Restored filtered track");
      listId = createManualList(runtime, "Restored filtered list", {trackId});
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target =
          rt::FilteredListTarget{
            .listId = listId,
            .filterExpression = "$title ~ \"Restored\"",
          },
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = presentation("songs"),
          },
      }));
      drainGtkEvents();
      REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
      REQUIRE(runtime.savePlaybackSession());
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
      savePresentationPreference(tempDir, listId, "albums");
      runtime.playback().commands().stop();
    }

    auto runtime = makeRuntime(tempDir);
    rt::test::addReadyAudioProvider(runtime);
    auto window = MainWindow{runtime, appConfigStore(tempDir), nullptr};
    window.initializeSession();
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
        CHECK(state.presentation.id == "albums");
      }
      else
      {
        ++filteredCount;
        CHECK(state.filterExpression == "$title ~ \"Restored\"");
        CHECK(state.presentation.id == "songs");
      }
    }

    CHECK(filteredCount == 1);
    CHECK(plainCount == 1);
  }

  TEST_CASE("MainWindowCoordinator - playback restore creates a preferred plain view when no target view exists",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;
    auto trackId = kInvalidTrackId;

    {
      auto runtime = makeRuntime(tempDir);
      rt::test::addReadyAudioProvider(runtime);
      trackId = addPlayableTrack(runtime, "Restored new-view track");
      listId = createManualList(runtime, "Restored new-view list", {trackId});
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
      REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
      REQUIRE(runtime.savePlaybackSession());
      savePresentationPreference(tempDir, listId, "albums");
      runtime.playback().commands().stop();
    }

    auto runtime = makeRuntime(tempDir);
    rt::test::addReadyAudioProvider(runtime);
    auto window = MainWindow{runtime, appConfigStore(tempDir), nullptr};
    window.initializeSession();
    drainGtkEvents();

    auto const listViews = viewsForList(runtime, listId);
    REQUIRE(listViews.size() == 1);
    auto const state = runtime.views().trackListState(listViews.front());
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation.id == "albums");
  }
} // namespace ao::gtk::test
