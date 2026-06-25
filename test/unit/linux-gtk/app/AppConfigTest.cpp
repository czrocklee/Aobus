// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AppConfig.h"

#include "app/UIState.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/StateTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace ao::gtk::test
{
  TEST_CASE("AppConfig persistence and operations", "[app][unit][config]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = std::filesystem::path{tempDir.path()} / "config.yaml";

    SECTION("Load non-existent config returns default values")
    {
      auto const config = AppConfig{configPath};

      auto windowState = WindowState{};
      windowState.width = 1200;
      windowState.height = 800;
      config.loadWindow(windowState);

      // Default loaded when not found should not modify the values
      CHECK(windowState.width == 1200);
      CHECK(windowState.height == 800);
    }

    SECTION("Save and load WindowState")
    {
      auto config = AppConfig{configPath};

      auto saveState = WindowState{};
      saveState.width = 1440;
      saveState.height = 900;
      saveState.maximized = true;

      config.saveWindow(saveState);

      auto const config2 = AppConfig{configPath};
      auto loadState = WindowState{};
      config2.loadWindow(loadState);

      CHECK(loadState.width == 1440);
      CHECK(loadState.height == 900);
      CHECK(loadState.maximized == true);
    }

    SECTION("Save and load AppPrefsState")
    {
      auto config = AppConfig{configPath};

      auto savePrefs = rt::AppPrefsState{};
      savePrefs.lastBackend = "test-backend";
      savePrefs.lastLayoutPreset = "modern";
      savePrefs.lastThemePreset = "modern";
      config.saveAppPrefs(savePrefs);

      auto const config2 = AppConfig{configPath};
      auto loadPrefs = rt::AppPrefsState{};
      config2.loadAppPrefs(loadPrefs);

      CHECK(loadPrefs.lastBackend == "test-backend");
      CHECK(loadPrefs.lastLayoutPreset == "modern");
      CHECK(loadPrefs.lastThemePreset == "modern");
    }
  }
} // namespace ao::gtk::test
