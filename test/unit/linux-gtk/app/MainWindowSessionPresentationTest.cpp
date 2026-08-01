// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/AppConfigStore.h"
#include "app/GtkLayoutStateStore.h"
#include "app/MainWindow.h"
#include "app/MainWindowCoordinator.h"
#include "list/ListNavigationController.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
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

    ListId createList(rt::AppRuntime& runtime, std::string name)
    {
      return ao::test::requireValue(runtime.library().writer().createList(rt::LibraryWriter::ListDraft{
        .name = std::move(name),
      }));
    }

    TrackId addPlayableTrack(rt::AppRuntime& runtime, std::string_view const title)
    {
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      return addRuntimeTrack(runtime, library::test::TrackSpec{.title = std::string{title}, .uri = fixturePath});
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

  TEST_CASE("MainWindowCoordinator - restored workspace presentation survives startup and history replay",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto otherListId = kInvalidListId;

    {
      auto runtimePtr = makeRuntime(tempDir);
      otherListId = createList(*runtimePtr, "History destination");
      REQUIRE(runtimePtr->workspace().navigate({
        .target = rt::kAllTracksListId,
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = presentation(rt::kListOrderTrackPresentationId),
          },
      }));
      runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
      savePresentationPreference(tempDir, rt::kAllTracksListId, "albums");
    }

    auto runtimePtr = makeRuntime(tempDir);
    auto window = MainWindow{*runtimePtr, appConfigStore(tempDir), nullptr};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto state = runtimePtr->views().trackListState(runtimePtr->workspace().snapshot().activeViewId);
    REQUIRE(state.listId == rt::kAllTracksListId);
    CHECK(state.presentation.id == rt::kListOrderTrackPresentationId);

    REQUIRE(runtimePtr->workspace().navigate({.target = otherListId}));
    REQUIRE(runtimePtr->workspace().goBack());
    state = runtimePtr->views().trackListState(runtimePtr->workspace().snapshot().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.presentation.id == rt::kListOrderTrackPresentationId);
  }

  TEST_CASE("MainWindowCoordinator - empty workspace creates All Tracks with its saved preference",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};

    {
      auto runtimePtr = makeRuntime(tempDir);
      savePresentationPreference(tempDir, rt::kAllTracksListId, "songs");
    }

    auto runtimePtr = makeRuntime(tempDir);
    auto window = MainWindow{*runtimePtr, appConfigStore(tempDir), nullptr};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto const state = runtimePtr->views().trackListState(runtimePtr->workspace().snapshot().activeViewId);
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
      auto runtimePtr = makeRuntime(tempDir);
      listId = createList(*runtimePtr, "Preferred list");
      savePresentationPreference(tempDir, listId, "albums");
    }

    auto runtimePtr = makeRuntime(tempDir);
    auto configStorePtr = appConfigStore(tempDir);
    auto window = Gtk::Window{};
    auto coordinator = MainWindowCoordinator{window, *runtimePtr, configStorePtr};
    coordinator.loadSession();
    coordinator.prepareSession();

    coordinator.listNavigationController()->select(listId);
    drainGtkEvents();
    auto const firstViewId = runtimePtr->workspace().snapshot().activeViewId;
    REQUIRE(firstViewId != rt::kInvalidViewId);
    CHECK(runtimePtr->views().trackListState(firstViewId).presentation.id == "albums");

    REQUIRE(runtimePtr->workspace().setActivePresentation(presentation("songs")));
    coordinator.listNavigationController()->select(rt::kAllTracksListId);
    drainGtkEvents();
    coordinator.listNavigationController()->select(listId);
    drainGtkEvents();

    CHECK(runtimePtr->workspace().snapshot().activeViewId == firstViewId);
    CHECK(runtimePtr->views().trackListState(firstViewId).presentation.id == "songs");
  }

  TEST_CASE("MainWindowCoordinator - playback restore reuses a restored plain view without changing presentation",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;
    auto trackId = kInvalidTrackId;

    {
      auto runtimePtr = makeRuntime(tempDir);
      rt::test::addReadyAudioProvider(*runtimePtr);
      trackId = addPlayableTrack(*runtimePtr, "Restored plain track");
      listId = createList(*runtimePtr, "Restored plain list");
      auto const viewId = ao::test::requireValue(runtimePtr->workspace().navigate({
        .target = listId,
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = presentation("songs"),
          },
      }));
      REQUIRE(runtimePtr->playback().commands().startFromView(viewId, trackId));
      REQUIRE(waitForPlaybackSettlement(*runtimePtr, trackId));
      REQUIRE(runtimePtr->savePlaybackSession());
      runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
      savePresentationPreference(tempDir, listId, "albums");
      runtimePtr->playback().commands().stop();
    }

    auto runtimePtr = makeRuntime(tempDir);
    rt::test::addReadyAudioProvider(*runtimePtr);
    auto window = MainWindow{*runtimePtr, appConfigStore(tempDir), nullptr};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto const listViews = viewsForList(*runtimePtr, listId);
    REQUIRE(listViews.size() == 1);
    auto const state = runtimePtr->views().trackListState(listViews.front());
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation.id == "songs");
    CHECK(state.selection == std::vector<TrackId>{trackId});
  }

  TEST_CASE("MainWindow - replacement activation leaves stored playback identity idle and unrestored",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};

    {
      auto runtimePtr = makeRuntime(tempDir);
      rt::test::addReadyAudioProvider(*runtimePtr);
      auto const trackId = addPlayableTrack(*runtimePtr, "Old library playback");
      runtimePtr->reloadAllTracks();
      auto const viewId = ao::test::requireValue(runtimePtr->workspace().navigate({.target = rt::kAllTracksListId}));
      REQUIRE(runtimePtr->playback().commands().startFromView(viewId, trackId));
      REQUIRE(waitForPlaybackSettlement(*runtimePtr, trackId));
      REQUIRE(runtimePtr->savePlaybackSession());
      runtimePtr->playback().commands().stop();
    }

    auto runtimePtr = makeRuntime(tempDir);
    rt::test::addReadyAudioProvider(*runtimePtr);
    auto window = MainWindow{*runtimePtr, appConfigStore(tempDir), nullptr};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::StartIdle));

    auto const& snapshot = runtimePtr->playback().snapshot();
    CHECK(snapshot.transport.transport == audio::Transport::Idle);
    CHECK(snapshot.transport.nowPlaying.trackId == kInvalidTrackId);
    CHECK(snapshot.succession.currentTrackId == kInvalidTrackId);
  }

  TEST_CASE("MainWindowCoordinator - playback restore creates a preferred plain view beside a filtered view",
            "[gtk][regression][session-presentation]")
  {
    auto const appPtr = ensureGtkApplication();
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;

    {
      auto runtimePtr = makeRuntime(tempDir);
      rt::test::addReadyAudioProvider(*runtimePtr);
      auto const trackId = addPlayableTrack(*runtimePtr, "Restored filtered track");
      listId = createList(*runtimePtr, "Restored filtered list");
      auto const viewId = ao::test::requireValue(runtimePtr->workspace().navigate({
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
      REQUIRE(runtimePtr->playback().commands().startFromView(viewId, trackId));
      REQUIRE(waitForPlaybackSettlement(*runtimePtr, trackId));
      REQUIRE(runtimePtr->savePlaybackSession());
      runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
      savePresentationPreference(tempDir, listId, "albums");
      runtimePtr->playback().commands().stop();
    }

    auto runtimePtr = makeRuntime(tempDir);
    rt::test::addReadyAudioProvider(*runtimePtr);
    auto window = MainWindow{*runtimePtr, appConfigStore(tempDir), nullptr};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto const listViews = viewsForList(*runtimePtr, listId);
    REQUIRE(listViews.size() == 2);
    std::int32_t filteredCount = 0;
    std::int32_t plainCount = 0;

    for (auto const viewId : listViews)
    {
      if (auto const state = runtimePtr->views().trackListState(viewId); state.filterExpression.empty())
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
      auto runtimePtr = makeRuntime(tempDir);
      rt::test::addReadyAudioProvider(*runtimePtr);
      trackId = addPlayableTrack(*runtimePtr, "Restored new-view track");
      listId = createList(*runtimePtr, "Restored new-view list");
      auto const viewId = ao::test::requireValue(runtimePtr->workspace().navigate({.target = listId}));
      REQUIRE(runtimePtr->playback().commands().startFromView(viewId, trackId));
      REQUIRE(waitForPlaybackSettlement(*runtimePtr, trackId));
      REQUIRE(runtimePtr->savePlaybackSession());
      savePresentationPreference(tempDir, listId, "albums");
      runtimePtr->playback().commands().stop();
    }

    auto runtimePtr = makeRuntime(tempDir);
    rt::test::addReadyAudioProvider(*runtimePtr);
    auto window = MainWindow{*runtimePtr, appConfigStore(tempDir), nullptr};
    REQUIRE(window.prepareSession());
    REQUIRE(window.activateSession(MainWindow::PlaybackRestoreMode::Restore));
    drainGtkEvents();

    auto const listViews = viewsForList(*runtimePtr, listId);
    REQUIRE(listViews.size() == 1);
    auto const state = runtimePtr->views().trackListState(listViews.front());
    CHECK(state.filterExpression.empty());
    CHECK(state.presentation.id == "albums");
  }
} // namespace ao::gtk::test
