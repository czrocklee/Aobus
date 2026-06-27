// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfig.h"
#include "app/UIState.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/library/MusicLibrary.h>
#include <ao/rt/StateTypes.h>

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
    CHECK(actionMap->lookup_action("keyboard-shortcuts") != nullptr);
  }

  TEST_CASE("MainWindow - hide persists current library path", "[gtk][unit][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);

    auto window = MainWindow{fixture.runtime(), configPtr, nullptr};

    auto before = rt::AppPrefsState{};
    configPtr->loadAppPrefs(before);
    REQUIRE(before.lastLibraryPath.empty());

    window.on_hide();

    auto after = rt::AppPrefsState{};
    configPtr->loadAppPrefs(after);
    CHECK(after.lastLibraryPath == fixture.runtime().musicLibrary().rootPath().string());
  }
} // namespace ao::gtk::test
