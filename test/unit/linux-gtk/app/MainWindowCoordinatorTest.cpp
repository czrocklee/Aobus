// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "../GtkTestSupport.h"
#include "app/AppConfigStore.h"
#include "app/MainWindow.h"
#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCoordinator.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Transport.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackSessionState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/playback/queue/PlaybackQueueSession.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    TrackId addTrackWithTitle(rt::AppRuntime& runtime, std::string_view title)
    {
      return library::test::addTrack(runtime.musicLibrary(), {.title = std::string{title}});
    }

    void updateTrackTitle(rt::AppRuntime& runtime, TrackId trackId, std::string_view title)
    {
      library::test::updateTrackSpec(
        runtime.musicLibrary(), trackId, [&](library::test::TrackSpec& spec) { spec.title = std::string{title}; });
    }

    rt::PlaybackSessionState loadPlaybackSession(rt::AppRuntime& runtime)
    {
      auto session = rt::PlaybackSessionState{};
      REQUIRE(runtime.configStore().load(rt::kPlaybackSessionConfigGroup, session));
      return session;
    }

    void overwritePlaybackSession(rt::AppRuntime& runtime, TrackId trackId, std::uint64_t positionMs)
    {
      runtime.configStore().save(rt::kPlaybackSessionConfigGroup,
                                 rt::PlaybackSessionState{
                                   .sourceListId = rt::kAllTracksListId,
                                   .trackId = trackId,
                                   .positionMs = positionMs,
                                 });
      REQUIRE(runtime.configStore().flush());
    }
  } // namespace

  TEST_CASE("MainWindowCoordinator - saveSession does not clobber explicit preferences", "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto initialPrefs = rt::AppPrefsState{};
    initialPrefs.lastThemePreset = "modern";
    initialPrefs.lastOutputBackendId = "preference-backend";
    initialPrefs.lastOutputDeviceId = "preference-device";
    initialPrefs.lastOutputProfileId = "preference-profile";
    configStorePtr->saveAppPrefs(initialPrefs);

    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};
    coordinator.loadSession();

    coordinator.themeController()->setTheme(rt::ThemePresetId::Classic);
    coordinator.saveSession();

    auto loadedPrefs = rt::AppPrefsState{};
    configStorePtr->loadAppPrefs(loadedPrefs);
    CHECK(loadedPrefs.lastThemePreset == "modern");
    CHECK(loadedPrefs.lastOutputBackendId == "preference-backend");
    CHECK(loadedPrefs.lastOutputDeviceId == "preference-device");
    CHECK(loadedPrefs.lastOutputProfileId == "preference-profile");

    auto loadedSession = rt::AppSessionState{};
    configStorePtr->loadAppSession(loadedSession);
    CHECK(loadedSession.lastLibraryPath == runtime.musicLibrary().rootPath().string());
  }

  TEST_CASE("MainWindowCoordinator - import mutation refreshes cached track rows", "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    auto const trackId = addTrackWithTitle(runtime, "Before Import");
    auto const rowBeforePtr = coordinator.trackRowCache()->trackRow(trackId);
    REQUIRE(rowBeforePtr);
    CHECK(rowBeforePtr->fieldText(rt::TrackField::Title) == "Before Import");

    updateTrackTitle(runtime, trackId, "After Import");
    CHECK(coordinator.trackRowCache()->trackRow(trackId)->fieldText(rt::TrackField::Title) == "Before Import");

    coordinator.importExport().callbacks().onLibraryDataMutated();

    auto const rowAfterPtr = coordinator.trackRowCache()->trackRow(trackId);
    REQUIRE(rowAfterPtr);
    CHECK(rowAfterPtr->fieldText(rt::TrackField::Title) == "After Import");
  }

  TEST_CASE("MainWindowCoordinator - partial output preferences fall back to session output",
            "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime.playback());

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);

    auto prefs = rt::AppPrefsState{};
    prefs.lastOutputBackendId = "incomplete-preference-backend";
    prefs.lastOutputProfileId = {};
    configStorePtr->saveAppPrefs(prefs);

    auto session = rt::AppSessionState{};
    session.lastOutputBackendId = "test_backend";
    session.lastOutputDeviceId = "test_device";
    session.lastOutputProfileId = audio::kProfileShared.raw();
    configStorePtr->saveAppSession(session);

    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};
    coordinator.loadSession();

    auto const& selected = runtime.playback().state().output.selectedDevice;
    CHECK(selected.backendId == audio::BackendId{"test_backend"});
    CHECK(selected.deviceId == audio::DeviceId{"test_device"});
    CHECK(selected.profileId == audio::kProfileShared);
  }

  TEST_CASE("MainWindowCoordinator - restores playback session as idle queue state",
            "[gtk][unit][main-window-playback][session]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const trackId = addTrackWithTitle(runtime, "Restored Track");
    auto session = rt::PlaybackSessionState{
      .sourceListId = ListId{9999},
      .trackId = trackId,
      .positionMs = 6789,
      .shuffleMode = rt::ShuffleMode::On,
      .repeatMode = rt::RepeatMode::All,
    };
    runtime.configStore().save(rt::kPlaybackSessionConfigGroup, session);
    REQUIRE(runtime.configStore().flush());

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    coordinator.initializeSession();

    auto* const queueSession = coordinator.playbackQueueSession();
    REQUIRE(queueSession != nullptr);
    CHECK(queueSession->isActive());
    CHECK(queueSession->nowPlayingTrackId() == trackId);
    CHECK(queueSession->sourceListId() == rt::kAllTracksListId);
    CHECK(runtime.playback().state().transport == audio::Transport::Idle);
    CHECK(runtime.playback().state().nowPlaying.trackId == trackId);
    CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{6789});
    CHECK(runtime.playback().state().mode.shuffle == rt::ShuffleMode::On);
    CHECK(runtime.playback().state().mode.repeat == rt::RepeatMode::All);

    auto const focusedViewId = runtime.workspace().layoutState().activeViewId;
    REQUIRE(focusedViewId != rt::kInvalidViewId);
    CHECK(runtime.views().trackListState(focusedViewId).selection == std::vector<TrackId>{trackId});
  }

  TEST_CASE("MainWindowCoordinator - persists playback session from playback events",
            "[gtk][unit][main-window-playback][session]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const track1 = addTrackWithTitle(runtime, "Restored Track");
    auto const track2 = addTrackWithTitle(runtime, "Changed Track");
    auto session = rt::PlaybackSessionState{
      .sourceListId = rt::kAllTracksListId,
      .trackId = track1,
      .positionMs = 100,
    };
    runtime.configStore().save(rt::kPlaybackSessionConfigGroup, session);
    REQUIRE(runtime.configStore().flush());

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configStorePtr = std::make_shared<AppConfigStore>(configPath);
    auto window = MainWindow{runtime, configStorePtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configStorePtr};

    coordinator.initializeSession();

    overwritePlaybackSession(runtime, track1, 1);
    runtime.playback().seek(std::chrono::milliseconds{250});
    auto saved = loadPlaybackSession(runtime);
    CHECK(saved.trackId == track1);
    CHECK(saved.positionMs == 250);

    overwritePlaybackSession(runtime, track1, 2);
    REQUIRE(runtime.playback().restoreSession(rt::PlaybackSessionState{
      .sourceListId = rt::kAllTracksListId,
      .trackId = track2,
      .positionMs = 400,
    }));
    saved = loadPlaybackSession(runtime);
    CHECK(saved.trackId == track2);
    CHECK(saved.positionMs == 400);

    runtime.playback().seek(std::chrono::milliseconds{550});
    overwritePlaybackSession(runtime, track1, 3);
    runtime.playback().stop();
    saved = loadPlaybackSession(runtime);
    CHECK(saved.trackId == track2);
    CHECK(saved.positionMs == 550);

    REQUIRE(runtime.playback().restoreSession(rt::PlaybackSessionState{
      .sourceListId = rt::kAllTracksListId,
      .trackId = track1,
      .positionMs = 700,
    }));
    overwritePlaybackSession(runtime, track2, 4);
    coordinator.saveSession();
    saved = loadPlaybackSession(runtime);
    CHECK(saved.trackId == track1);
    CHECK(saved.positionMs == 700);
  }
} // namespace ao::gtk::test
