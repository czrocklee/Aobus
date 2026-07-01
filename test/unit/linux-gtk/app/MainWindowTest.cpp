// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfig.h"
#include "app/UIState.h"
#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Backend.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppPrefsState.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/actionmap.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::gtk::test
{
  TEST_CASE("MainWindow - constructs shell with title and window actions", "[gtk][unit][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);
    configPtr->saveWindow(WindowState{.width = 640, .height = 480, .maximized = false});

    auto window = MainWindow{fixture.runtime(), configPtr, nullptr};

    CHECK(window.get_title() == "Aobus");

    std::int32_t defaultWidth = 0;
    std::int32_t defaultHeight = 0;
    window.get_default_size(defaultWidth, defaultHeight);
    CHECK(defaultWidth == 640);
    CHECK(defaultHeight == 480);

    auto* const actionMap = dynamic_cast<Gio::ActionMap*>(&window);
    REQUIRE(actionMap != nullptr);
    CHECK(actionMap->lookup_action("open-library") != nullptr);
    CHECK(actionMap->lookup_action("scan-library") != nullptr);
    CHECK(actionMap->lookup_action("import-library") != nullptr);
    CHECK(actionMap->lookup_action("export-library") != nullptr);
    CHECK(actionMap->lookup_action("edit-layout") != nullptr);
    CHECK(actionMap->lookup_action("reset-runtime-layout-state") != nullptr);
    CHECK(actionMap->lookup_action("save-panel-sizes-as-layout-defaults") != nullptr);
    CHECK(actionMap->lookup_action("keyboard-shortcuts") == nullptr);
  }

  TEST_CASE("MainWindow - hide persists current library path", "[gtk][unit][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);

    auto window = MainWindow{fixture.runtime(), configPtr, nullptr};

    auto before = rt::AppSessionState{};
    configPtr->loadAppSession(before);
    REQUIRE(before.lastLibraryPath.empty());

    window.on_hide();

    auto after = rt::AppSessionState{};
    configPtr->loadAppSession(after);
    CHECK(after.lastLibraryPath == fixture.runtime().musicLibrary().rootPath().string());
  }

  TEST_CASE("MainWindow - explicit session save persists current library path", "[gtk][unit][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);

    auto window = MainWindow{fixture.runtime(), configPtr, nullptr};

    auto before = rt::AppSessionState{};
    configPtr->loadAppSession(before);
    REQUIRE(before.lastLibraryPath.empty());

    window.saveSession();

    auto after = rt::AppSessionState{};
    configPtr->loadAppSession(after);
    CHECK(after.lastLibraryPath == fixture.runtime().musicLibrary().rootPath().string());
  }

  TEST_CASE("MainWindow - restores saved output when audio provider is bootstrapped before session load",
            "[gtk][unit][main-window][audio]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);
    auto prefs = rt::AppPrefsState{};
    prefs.lastOutputBackendId = "test_backend";
    prefs.lastOutputDeviceId = "test_device";
    prefs.lastOutputProfileId = audio::kProfileShared.raw();
    configPtr->saveAppPrefs(prefs);

    rt::test::addReadyAudioProvider(fixture.runtime().playback());

    auto window = MainWindow{fixture.runtime(), configPtr, nullptr};
    drainGtkEvents();

    auto const& output = fixture.runtime().playback().state().selectedOutputDevice;
    CHECK(output.backendId == audio::BackendId{"test_backend"});
    CHECK(output.deviceId == audio::DeviceId{"test_device"});
    CHECK(output.profileId == audio::kProfileShared);
  }
} // namespace ao::gtk::test
