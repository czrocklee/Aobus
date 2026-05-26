// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../lmdb/TestUtils.h"
#include "../GtkTestSupport.h"
#include "playback/SeekControl.h"
#include "playback/TimeLabel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/adjustment.h>  // NOLINT(misc-include-cleaner)
#include <gtkmm/application.h> // NOLINT(misc-include-cleaner)
#include <gtkmm/label.h>
#include <gtkmm/scale.h>

#include <memory>

namespace ao::gtk::test
{
  using namespace ao::lmdb::test;

  namespace
  {
    struct TestEnvironment final
    {
      TempDir tempDir{};
      std::shared_ptr<rt::ConfigStore> configStore;
      rt::AppRuntime runtime;

      TestEnvironment()
        : configStore{std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml")}
        , runtime{
            rt::AppRuntimeDependencies{.executor = std::make_unique<ImmediateExecutor>(),
                                       .musicRoot = tempDir.path(),
                                       .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                       .workspaceConfigStore = configStore}}
      {
      }
    };
  } // namespace

  TEST_CASE("Playback UI Components - Smoke Tests", "[playback][unit][ui]")
  {
    // GTK initialization for widget creation
    [[maybe_unused]] auto const app = Gtk::Application::create("io.github.aobus.ui_test");
    auto env = TestEnvironment{};
    auto& playback = env.runtime.playback();

    SECTION("SeekControl and TimeLabel initialization")
    {
      auto seekControl = SeekControl{playback};
      auto timeLabel = TimeLabel{playback};

      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
      auto* const label = dynamic_cast<Gtk::Label*>(&timeLabel.widget());

      REQUIRE(scale != nullptr);
      REQUIRE(label != nullptr);
    }
  }
} // namespace ao::gtk::test
