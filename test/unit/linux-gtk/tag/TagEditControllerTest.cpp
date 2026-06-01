// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditController.h"

#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/application.h>
#include <gtkmm/window.h>

#include <memory>
#include <utility>

namespace ao::gtk::test
{
  TEST_CASE("TagEditController - smoke test", "[gtk][tag][controller]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};

    auto themeController = ThemeCoordinator{};
    auto callbacks = TagEditController::Callbacks{.onTagsMutated = [] {}};

    auto controller = TagEditController{window, fixture.runtime(), std::move(callbacks), themeController};

    SECTION("setup and addActionsTo does not crash")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);

      auto addActionPtr = std::dynamic_pointer_cast<Gio::SimpleAction>(groupPtr->lookup_action("track-tag-add"));
      REQUIRE(addActionPtr);

      // We don't have an active selection here, so it will return early without crashing.
      addActionPtr->activate(Glib::Variant<Glib::ustring>::create("ActionTag"));
      drainGtkEvents();
    }
  }
} // namespace ao::gtk::test
