// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCustomViewDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackCustomViewDialog - lifecycle", "[gtk][track][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};

    auto spec = rt::TrackPresentationSpec{};
    spec.visibleFields = {rt::TrackField::Title};

    SECTION("dialog creation")
    {
      auto dialog = TrackCustomViewDialog{window, spec, "Initial Label"};
      drainGtkEvents();

      auto width = 0;
      auto height = 0;
      dialog.get_default_size(width, height);

      CHECK(width == -1);
      CHECK(height == -1);

      auto scrolledWindows = std::vector<Gtk::ScrolledWindow*>{};
      auto* const child = dialog.get_child();
      REQUIRE(child != nullptr);
      collectScrolledWindows(*child, scrolledWindows);

      auto foundContentScroll = false;

      for (auto* const scrolledWindow : scrolledWindows)
      {
        if (scrolledWindow->get_min_content_width() == 480)
        {
          foundContentScroll = true;
          CHECK(scrolledWindow->get_propagate_natural_width());
          CHECK(scrolledWindow->get_propagate_natural_height());
          CHECK(scrolledWindow->get_max_content_width() == 700);
          CHECK(scrolledWindow->get_max_content_height() == 520);
        }
      }

      CHECK(foundContentScroll);
    }
  }
} // namespace ao::gtk::test
