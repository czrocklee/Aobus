// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/StatusSlot.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/library/LibraryScanner.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryTasks.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/widget.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <utility>

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

    void iterateOneGtkEvent()
    {
      if (auto contextPtr = Glib::MainContext::get_default(); contextPtr->pending())
      {
        contextPtr->iteration(false);
      }
    }

    void waitForFuture(std::future<void>& future)
    {
      for (std::int32_t attempts = 0; attempts < 1000; ++attempts)
      {
        iterateOneGtkEvent();

        if (future.wait_for(std::chrono::milliseconds{1}) == std::future_status::ready)
        {
          future.get();
          drainGtkEvents();
          return;
        }
      }

      FAIL("Timed out waiting for library task");
    }

    template<typename T>
    T waitForFuture(std::future<T>& future)
    {
      for (std::int32_t attempts = 0; attempts < 1000; ++attempts)
      {
        iterateOneGtkEvent();

        if (future.wait_for(std::chrono::milliseconds{1}) == std::future_status::ready)
        {
          auto value = future.get();
          drainGtkEvents();
          return value;
        }
      }

      FAIL("Timed out waiting for library task");
      return T{};
    }

    template<typename T>
    bool waitUntilProgressVisible(StatusSlotWidgets const& widgets, std::future<T>& future)
    {
      for (std::int32_t attempts = 0; attempts < 1000; ++attempts)
      {
        iterateOneGtkEvent();

        if (widgets.progress->get_visible())
        {
          return true;
        }

        if (future.wait_for(std::chrono::milliseconds{1}) == std::future_status::ready)
        {
          return widgets.progress->get_visible();
        }
      }

      return false;
    }
  } // namespace

  TEST_CASE("StatusSlot - renders runtime status events", "[gtk][track][status]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto slot = StatusSlot{runtime.library().changes(), runtime.notifications(), runtime.views()};
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
      auto const path = runtime.musicRoot() / "status-slot.flac";
      std::ofstream{path} << "audio fixture";

      auto future = runtime.async().spawn(runtime.library().tasks().buildScanPlanAsync());
      REQUIRE(waitUntilProgressVisible(widgets, future));

      CHECK_FALSE(widgets.selection->get_visible());
      CHECK(widgets.message->get_visible());
      CHECK(widgets.progress->get_visible());
      CHECK(widgets.message->get_text() == "Scanning: status-slot.flac");
      CHECK(widgets.progress->get_fraction() == 0.0);

      auto const plan = waitForFuture(future);
      REQUIRE(plan.items.size() == 1);
      CHECK(plan.items[0].classification == library::ScanClassification::New);
    }

    SECTION("library completion renders the completion message")
    {
      runtime.notifications().post(
        rt::NotificationSeverity::Info, "Saved playlist", false, std::chrono::milliseconds{5000});
      drainGtkEvents();

      auto plan = library::ScanPlan{};
      auto future = runtime.async().spawn(runtime.library().tasks().applyScanPlanAsync(std::move(plan)));
      waitForFuture(future);

      CHECK_FALSE(widgets.selection->get_visible());
      CHECK(widgets.message->get_visible());
      CHECK_FALSE(widgets.progress->get_visible());
      CHECK(widgets.message->get_text() == "Library is up to date");
      CHECK(widgets.message->has_css_class("ao-status-info"));
    }
  }
} // namespace ao::gtk::test
