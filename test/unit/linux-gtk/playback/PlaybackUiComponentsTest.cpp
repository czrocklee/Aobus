// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../GtkTestSupport.h"
#include "playback/SeekControl.h"
#include "playback/TimeLabel.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/AppRuntime.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/adjustment.h>  // NOLINT(misc-include-cleaner)
#include <gtkmm/application.h> // NOLINT(misc-include-cleaner)
#include <gtkmm/label.h>
#include <gtkmm/scale.h>

#include <cstdint>

namespace ao::gtk::test
{
  namespace
  {
    struct TestEnvironment final
    {
      ao::test::TempDir tempDir{};
      rt::AppRuntime runtime;

      TestEnvironment()
        : runtime{makeRuntime(tempDir)}
      {
      }
    };
  } // namespace

  TEST_CASE("Playback UI components render initial GTK bindings", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = TestEnvironment{};
    auto& playback = env.runtime.playback();

    SECTION("SeekControl renders a disabled seek scale before playback starts")
    {
      auto seekControl = SeekControl{playback};

      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
      REQUIRE(scale != nullptr);
      CHECK_FALSE(scale->get_sensitive());
      CHECK(scale->has_css_class("ao-seekbar"));
      CHECK(scale->get_value() == 0.0);
      CHECK(scale->get_adjustment()->get_upper() == 100.0);
    }

    SECTION("TimeLabel renders the playback time template before playback starts")
    {
      auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Default};

      auto* const label = dynamic_cast<Gtk::Label*>(&timeLabel.widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "00:00 / 00:00");
      CHECK(label->has_css_class("ao-time-label"));

      std::int32_t widthRequest = 0;
      std::int32_t heightRequest = 0;
      label->get_size_request(widthRequest, heightRequest);
      CHECK(widthRequest > 0);
    }
  }
} // namespace ao::gtk::test
