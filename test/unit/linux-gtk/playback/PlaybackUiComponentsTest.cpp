// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../lmdb/TestUtils.h"
#include "../GtkTestSupport.h"
#include "playback/SeekControl.h"
#include "playback/TimeLabel.h"
#include <ao/rt/AppRuntime.h>
#include <catch2/catch_test_macros.hpp>
#include <gtkmm/adjustment.h>  // NOLINT(misc-include-cleaner)
#include <gtkmm/application.h> // NOLINT(misc-include-cleaner)
#include <gtkmm/label.h>
#include <gtkmm/scale.h>

namespace ao::gtk::test
{
  using namespace ao::lmdb::test;

  namespace
  {
    struct TestEnvironment final
    {
      TempDir tempDir{};
      rt::AppRuntime runtime;

      TestEnvironment()
        : runtime{makeRuntime(tempDir)}
      {
      }
    };
  } // namespace

  TEST_CASE("Playback UI Components - Smoke Tests", "[playback][unit][ui]")
  {
    // GTK initialization for widget creation
    [[maybe_unused]] auto const appPtr = Gtk::Application::create("io.github.aobus.ui_test");
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
