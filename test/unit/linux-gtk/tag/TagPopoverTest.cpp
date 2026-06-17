// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagPopover.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TagPopover - lifecycle", "[gtk][tag][popover]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();
    auto window = Gtk::Window{};

    SECTION("initialization")
    {
      auto popover = TagPopover{library, fixture.runtime().completion(), {}};
      popover.set_parent(window);

      // Test signal accessor
      auto connected = false;
      popover.signalTagsChanged().connect([&](auto, auto) { connected = true; });
    }
  }
} // namespace ao::gtk::test
