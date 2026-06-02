// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindowCoordinator.h"

#include "../GtkTestSupport.h"
#include "app/AppConfig.h"
#include "app/MainWindow.h"
#include "app/ThemeCoordinator.h"
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/StateTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace ao::gtk::test
{
  TEST_CASE("MainWindowCoordinator - saveSession preserves selected theme", "[gtk][app][main-window]")
  {
    auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);

    // Pre-populate config with some playback/library prefs
    auto initialPrefs = rt::AppPrefsState{};
    initialPrefs.lastLibraryPath = "/initial/path";
    initialPrefs.lastThemePreset = "classic";
    configPtr->saveAppPrefs(initialPrefs);

    auto window = MainWindow{runtime, configPtr};
    auto coordinator = MainWindowCoordinator{window, runtime, configPtr};
    coordinator.loadSession();

    // Set theme to modern
    coordinator.themeController()->setTheme(rt::ThemePresetId::Modern);

    // Save session
    coordinator.saveSession();

    // Reload and verify
    auto loaded = rt::AppPrefsState{};
    configPtr->loadAppPrefs(loaded);

    CHECK(loaded.lastThemePreset == "modern");
    CHECK(loaded.lastLibraryPath == runtime.musicLibrary().rootPath().string());
  }
} // namespace ao::gtk::test
