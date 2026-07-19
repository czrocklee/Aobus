// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AppConfigStore.h"

#include "app/WindowState.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/AppPrefsState.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace ao::gtk::test
{
  TEST_CASE("AppConfigStore - persists session and application preferences", "[gtk][unit][app][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = std::filesystem::path{tempDir.path()} / "config.yaml";

    SECTION("Load non-existent config returns default values")
    {
      auto const configStore = AppConfigStore{configPath};

      auto windowState = WindowState{};
      windowState.width = 1200;
      windowState.height = 800;
      configStore.loadWindow(windowState);

      // Default loaded when not found should not modify the values
      CHECK(windowState.width == 1200);
      CHECK(windowState.height == 800);
    }

    SECTION("Save and load WindowState")
    {
      auto configStore = AppConfigStore{configPath};

      auto saveState = WindowState{};
      saveState.width = 1440;
      saveState.height = 900;
      saveState.maximized = true;

      configStore.saveWindow(saveState);

      auto const restoredStore = AppConfigStore{configPath};
      auto loadState = WindowState{};
      restoredStore.loadWindow(loadState);

      CHECK(loadState.width == 1440);
      CHECK(loadState.height == 900);
      CHECK(loadState.maximized == true);
    }

    SECTION("Save and load AppPrefsState")
    {
      auto configStore = AppConfigStore{configPath};

      auto savePrefs = rt::AppPrefsState{};
      savePrefs.lastOutputBackendId = "test-backend";
      savePrefs.lastLayoutPreset = "modern";
      savePrefs.lastThemePreset = "modern";
      configStore.saveAppPrefs(savePrefs);

      auto const restoredStore = AppConfigStore{configPath};
      auto loadPrefs = rt::AppPrefsState{};
      restoredStore.loadAppPrefs(loadPrefs);

      CHECK(loadPrefs.lastOutputBackendId == "test-backend");
      CHECK(loadPrefs.lastLayoutPreset == "modern");
      CHECK(loadPrefs.lastThemePreset == "modern");
    }

    SECTION("Save and load AppSessionState")
    {
      auto configStore = AppConfigStore{configPath};

      auto saveSession = rt::AppSessionState{};
      saveSession.lastLibraryPath = "/tmp/music";
      saveSession.lastOutputBackendId = "session-backend";
      saveSession.lastOutputDeviceId = "session-device";
      saveSession.lastOutputProfileId = "session-profile";
      configStore.saveAppSession(saveSession);

      auto const restoredStore = AppConfigStore{configPath};
      auto loadSession = rt::AppSessionState{};
      restoredStore.loadAppSession(loadSession);

      CHECK(loadSession.lastLibraryPath == "/tmp/music");
      CHECK(loadSession.lastOutputBackendId == "session-backend");
      CHECK(loadSession.lastOutputDeviceId == "session-device");
      CHECK(loadSession.lastOutputProfileId == "session-profile");
    }

    SECTION("Partial window groups retain seeded fields and allow unknown keys")
    {
      auto output = std::ofstream{configPath};
      output << "window:\n"
                "  width: 1200\n"
                "  futurePlacementPolicy: centered\n";
      output.close();

      auto const configStore = AppConfigStore{configPath};
      auto state = WindowState{.width = 640, .height = 480, .maximized = true};
      configStore.loadWindow(state);

      CHECK(state.width == 1200);
      CHECK(state.height == 480);
      CHECK(state.maximized);
    }

    SECTION("Malformed known fields reject the whole group")
    {
      auto output = std::ofstream{configPath};
      output << "window:\n"
                "  width: 1200\n"
                "  height: malformed\n"
                "  maximized: false\n";
      output.close();

      auto const configStore = AppConfigStore{configPath};
      auto state = WindowState{.width = 640, .height = 480, .maximized = true};
      configStore.loadWindow(state);

      CHECK(state.width == 640);
      CHECK(state.height == 480);
      CHECK(state.maximized);
    }
  }
} // namespace ao::gtk::test
