// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MainWindow.h"

#include "app/AppConfig.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

namespace ao::gtk::test
{
  // Full-window smoke only. Session/layout behavior is covered by
  // MainWindowCoordinatorTest and ShellLayoutControllerTest; mouse-button
  // navigation by MouseNavigationPolicyTest.
  TEST_CASE("MainWindow - constructs with default window properties", "[gtk][app][main-window]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto const configPath = std::filesystem::path{fixture.tempDir().path()} / "app_config.yaml";
    auto configPtr = std::make_shared<AppConfig>(configPath);

    auto window = MainWindow{fixture.runtime(), configPtr, nullptr};

    CHECK(window.get_title() == "Aobus");
  }
} // namespace ao::gtk::test
