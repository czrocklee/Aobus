// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/StatusSlot.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/widget.h>

#include <chrono>

namespace ao::gtk::test
{
  namespace
  {
    struct StatusSlotWidgets final
    {
      Gtk::Widget* selection = nullptr;
      Gtk::Label* message = nullptr;
      Gtk::ProgressBar* progress = nullptr;
    };

    StatusSlotWidgets inspect(StatusSlot& slot)
    {
      auto* const box = dynamic_cast<Gtk::Box*>(&slot.widget());
      REQUIRE(box != nullptr);

      auto* const selection = box->get_first_child();
      REQUIRE(selection != nullptr);

      auto* const message = dynamic_cast<Gtk::Label*>(selection->get_next_sibling());
      REQUIRE(message != nullptr);

      auto* const progress = dynamic_cast<Gtk::ProgressBar*>(message->get_next_sibling());
      REQUIRE(progress != nullptr);

      return StatusSlotWidgets{.selection = selection, .message = message, .progress = progress};
    }
  } // namespace

  TEST_CASE("StatusSlot - renders runtime status events", "[gtk][track][status]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto slot = StatusSlot{runtime.mutation(), runtime.notifications(), runtime.views()};
    auto const widgets = inspect(slot);

    SECTION("starts with selection info visible")
    {
      CHECK(widgets.selection->get_visible());
      CHECK_FALSE(widgets.message->get_visible());
      CHECK_FALSE(widgets.progress->get_visible());
    }

    SECTION("notification posts render as status messages")
    {
      runtime.notifications().post(rt::NotificationSeverity::Warning, "Partial import", true);
      drainGtkEvents();

      CHECK_FALSE(widgets.selection->get_visible());
      CHECK(widgets.message->get_visible());
      CHECK_FALSE(widgets.progress->get_visible());
      CHECK(widgets.message->get_text() == "Partial import");
      CHECK(widgets.message->has_css_class("ao-status-warning"));
    }

    SECTION("library progress renders the progress bar")
    {
      runtime.mutation().notifyLibraryTaskProgress({.fraction = 0.5, .message = "Scanning Music"});
      drainGtkEvents();

      CHECK_FALSE(widgets.selection->get_visible());
      CHECK(widgets.message->get_visible());
      CHECK(widgets.progress->get_visible());
      CHECK(widgets.message->get_text() == "Scanning Music");
      CHECK(widgets.progress->get_fraction() == 0.5);
    }

    SECTION("library completion renders the completion message")
    {
      runtime.notifications().post(
        rt::NotificationSeverity::Info, "Saved playlist", false, std::chrono::milliseconds{5000});
      drainGtkEvents();

      runtime.mutation().notifyLibraryTaskCompleted(5);
      drainGtkEvents();

      CHECK_FALSE(widgets.selection->get_visible());
      CHECK(widgets.message->get_visible());
      CHECK_FALSE(widgets.progress->get_visible());
      CHECK(widgets.message->get_text() == "Scan complete: 5 tracks added");
      CHECK(widgets.message->has_css_class("ao-status-info"));
    }
  }
} // namespace ao::gtk::test
