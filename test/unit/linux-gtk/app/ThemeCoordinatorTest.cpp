// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ThemeCoordinator.h"

#include "../GtkTestSupport.h"
#include "app/AppConfig.h"
#include <ao/rt/StateTypes.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

#include <utility>

namespace ao::gtk::test
{
  TEST_CASE("ThemeCoordinator - applies active theme to registered windows", "[gtk][app][theme]")
  {
    auto const appPtr = ensureGtkApplication();
    auto coordinator = ThemeCoordinator{};
    auto window = Gtk::Window{};

    auto const token = coordinator.registerToplevel(window);
    CHECK(window.has_css_class("ao-theme-classic"));

    coordinator.setTheme(rt::ThemePresetId::Modern);
    CHECK_FALSE(window.has_css_class("ao-theme-classic"));
    CHECK(window.has_css_class("ao-theme-modern"));
  }

  TEST_CASE("ThemeCoordinator - unregisters on token destruction", "[gtk][app][theme]")
  {
    auto const appPtr = ensureGtkApplication();
    auto coordinator = ThemeCoordinator{};
    auto window = Gtk::Window{};

    {
      auto token = coordinator.registerToplevel(window);
      CHECK(window.has_css_class("ao-theme-classic"));

      coordinator.setTheme(rt::ThemePresetId::Modern);
      CHECK(window.has_css_class("ao-theme-modern"));
    }

    // Token destroyed, window should be unregistered AND its active theme class removed
    CHECK_FALSE(window.has_css_class("ao-theme-modern"));

    coordinator.setTheme(rt::ThemePresetId::Classic);
    CHECK_FALSE(window.has_css_class("ao-theme-classic"));
  }

  TEST_CASE("ThemeCoordinator - move and reset tokens", "[gtk][app][theme]")
  {
    auto const appPtr = ensureGtkApplication();
    auto coordinator = ThemeCoordinator{};
    auto window = Gtk::Window{};

    auto token1 = coordinator.registerToplevel(window);
    CHECK(window.has_css_class("ao-theme-classic"));

    auto token2 = std::move(token1);
    token1.reset(); // NOLINT(bugprone-use-after-move) — intentional: verify moved-from token handles reset safely

    coordinator.setTheme(rt::ThemePresetId::Modern);
    CHECK(window.has_css_class("ao-theme-modern"));

    token2.reset();
    coordinator.setTheme(rt::ThemePresetId::Classic);
    CHECK_FALSE(window.has_css_class("ao-theme-classic"));
  }

  TEST_CASE("ThemeCoordinator - loads and saves app prefs", "[gtk][app][theme]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto config = AppConfig{std::filesystem::path{tempDir.path()} / "config.yaml"};

    auto coordinator = ThemeCoordinator{};
    coordinator.setTheme(rt::ThemePresetId::Modern);
    coordinator.save(config);

    auto loaded = rt::AppPrefsState{};
    config.loadAppPrefs(loaded);
    CHECK(loaded.lastThemePreset == "modern");

    auto restored = ThemeCoordinator{};
    restored.load(config);
    CHECK(restored.activeTheme() == rt::ThemePresetId::Modern);
  }
} // namespace ao::gtk::test
