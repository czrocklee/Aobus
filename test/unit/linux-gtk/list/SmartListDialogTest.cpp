// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/SmartListDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("SmartListDialog - smoke test", "[gtk][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().musicLibrary()};

    auto dialog = SmartListDialog{window, fixture.runtime(), rt::kAllTracksListId, cache};
    window.set_child(dialog);

    // Rebuild happens in idle task
    drainGtkEvents();
    drainGtkEvents();

    auto width = 0;
    auto height = 0;
    dialog.get_default_size(width, height);

    CHECK(width == -1);
    CHECK(height == -1);

    auto scrolledWindows = std::vector<Gtk::ScrolledWindow*>{};
    collectScrolledWindows(dialog, scrolledWindows);

    auto foundPreviewScroll = false;

    for (auto* const scrolledWindow : scrolledWindows)
    {
      if (scrolledWindow->get_min_content_width() == 420)
      {
        foundPreviewScroll = true;
        CHECK(scrolledWindow->get_propagate_natural_width());
        CHECK(scrolledWindow->get_propagate_natural_height());
        CHECK(scrolledWindow->get_min_content_height() == 360);
        CHECK(scrolledWindow->get_max_content_width() == 640);
        CHECK(scrolledWindow->get_max_content_height() == 520);
      }
    }

    CHECK(foundPreviewScroll);
  }

  TEST_CASE("SmartListDialog - populate for edit", "[gtk][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().musicLibrary()};

    auto dialog = SmartListDialog{window, fixture.runtime(), kInvalidListId, cache};
    window.set_child(dialog);

    drainGtkEvents();

    // Create a dummy ListView
  }
} // namespace ao::gtk::test
