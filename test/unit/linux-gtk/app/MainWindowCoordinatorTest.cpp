// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "../GtkTestSupport.h"
#include "app/AppConfig.h"
#include "app/MainWindow.h"
#include "app/ThemeCoordinator.h"
#include "portal/ImportExportCoordinator.h"
#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>

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
  } // namespace

  TEST_CASE("MainWindowCoordinator - saveSession does not clobber explicit preferences", "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);

    auto initialPrefs = rt::AppPrefsState{};
    initialPrefs.lastThemePreset = "modern";
    initialPrefs.lastOutputBackendId = "preference-backend";
    initialPrefs.lastOutputDeviceId = "preference-device";
    initialPrefs.lastOutputProfileId = "preference-profile";
    configPtr->saveAppPrefs(initialPrefs);

    auto window = MainWindow{runtime, configPtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configPtr};
    coordinator.loadSession();

    coordinator.themeController()->setTheme(rt::ThemePresetId::Classic);
    coordinator.saveSession();

    auto loadedPrefs = rt::AppPrefsState{};
    configPtr->loadAppPrefs(loadedPrefs);
    CHECK(loadedPrefs.lastThemePreset == "modern");
    CHECK(loadedPrefs.lastOutputBackendId == "preference-backend");
    CHECK(loadedPrefs.lastOutputDeviceId == "preference-device");
    CHECK(loadedPrefs.lastOutputProfileId == "preference-profile");

    auto loadedSession = rt::AppSessionState{};
    configPtr->loadAppSession(loadedSession);
    CHECK(loadedSession.lastLibraryPath == runtime.musicLibrary().rootPath().string());
  }

  TEST_CASE("MainWindowCoordinator - import mutation refreshes cached track rows", "[gtk][unit][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);

    auto window = MainWindow{runtime, configPtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configPtr};

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
    auto configPtr = std::make_shared<AppConfig>(configPath);

    auto prefs = rt::AppPrefsState{};
    prefs.lastOutputBackendId = "incomplete-preference-backend";
    prefs.lastOutputProfileId = {};
    configPtr->saveAppPrefs(prefs);

    auto session = rt::AppSessionState{};
    session.lastOutputBackendId = "test_backend";
    session.lastOutputDeviceId = "test_device";
    session.lastOutputProfileId = audio::kProfileShared.raw();
    configPtr->saveAppSession(session);

    auto window = MainWindow{runtime, configPtr, nullptr};
    auto coordinator = MainWindowCoordinator{window, runtime, configPtr};
    coordinator.loadSession();

    auto const& selected = runtime.playback().state().selectedOutputDevice;
    CHECK(selected.backendId == audio::BackendId{"test_backend"});
    CHECK(selected.deviceId == audio::DeviceId{"test_device"});
    CHECK(selected.profileId == audio::kProfileShared);
  }
} // namespace ao::gtk::test
