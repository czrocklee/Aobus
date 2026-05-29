// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/SmartListDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

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
