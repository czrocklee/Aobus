// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryTaskProgressDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::gtk::test
{
  using ao::gtk::portal::LibraryTaskProgressDialog;

  TEST_CASE("LibraryTaskProgressDialog - lifecycle", "[gtk][portal][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto parent = Gtk::Window{};

    SECTION("construction wires up the dialog with default state")
    {
      auto dialog = LibraryTaskProgressDialog{42, parent};
      drainGtkEvents();

      CHECK(dialog.get_title() == "Library Task Progress");
      CHECK(dialog.get_transient_for() == &parent);

      auto* const titlebar = dialog.get_titlebar();
      REQUIRE(titlebar != nullptr);
      auto* const headerBar = dynamic_cast<Gtk::HeaderBar*>(titlebar);
      REQUIRE(headerBar != nullptr);

      auto* const okButton = findButtonByLabel(*headerBar, "OK");
      REQUIRE(okButton != nullptr);
      CHECK_FALSE(okButton->get_sensitive());

      auto progressBars = collectAll<Gtk::ProgressBar>(dialog);
      REQUIRE(progressBars.size() == 1);
      CHECK(progressBars.front()->get_fraction() == 0.0);

      CHECK(findLabelByText(dialog, "Starting...") != nullptr);
    }

    SECTION("updateProgress updates the progress label and fraction")
    {
      auto dialog = LibraryTaskProgressDialog{42, parent};
      drainGtkEvents();

      dialog.updateProgress("Halfway through", 0.5);
      drainGtkEvents();

      auto progressBars = collectAll<Gtk::ProgressBar>(dialog);
      REQUIRE(progressBars.size() == 1);
      CHECK(progressBars.front()->get_fraction() == 0.5);

      CHECK(findLabelByText(dialog, "Halfway through") != nullptr);
    }

    SECTION("ready flips the title and enables the OK button")
    {
      auto dialog = LibraryTaskProgressDialog{42, parent};
      drainGtkEvents();

      dialog.ready();
      drainGtkEvents();

      auto* const titlebar = dialog.get_titlebar();
      REQUIRE(titlebar != nullptr);
      auto* const headerBar = dynamic_cast<Gtk::HeaderBar*>(titlebar);
      REQUIRE(headerBar != nullptr);

      auto* const okButton = findButtonByLabel(*headerBar, "OK");
      REQUIRE(okButton != nullptr);
      CHECK(okButton->get_sensitive());

      auto progressBars = collectAll<Gtk::ProgressBar>(dialog);
      REQUIRE(progressBars.size() == 1);
      CHECK(progressBars.front()->get_fraction() == 1.0);

      CHECK(findLabelByText(dialog, "All items processed.") != nullptr);
    }

    SECTION("beginTask restores the in-progress state so the dialog can be reused")
    {
      auto dialog = LibraryTaskProgressDialog{42, parent};
      drainGtkEvents();

      dialog.updateProgress("Updating: foo", 0.5);
      dialog.ready();
      drainGtkEvents();

      auto* const titlebar = dialog.get_titlebar();
      REQUIRE(titlebar != nullptr);
      auto* const headerBar = dynamic_cast<Gtk::HeaderBar*>(titlebar);
      REQUIRE(headerBar != nullptr);
      auto* const okButton = findButtonByLabel(*headerBar, "OK");
      REQUIRE(okButton != nullptr);
      REQUIRE(okButton->get_sensitive());

      dialog.beginTask();
      drainGtkEvents();

      CHECK_FALSE(okButton->get_sensitive());

      auto progressBars = collectAll<Gtk::ProgressBar>(dialog);
      REQUIRE(progressBars.size() == 1);
      CHECK(progressBars.front()->get_fraction() == 0.0);

      CHECK(findLabelByText(dialog, "Starting...") != nullptr);
      CHECK(findLabelByText(dialog, "All items processed.") == nullptr);
    }

    SECTION("clicking OK emits the response signal")
    {
      auto dialog = LibraryTaskProgressDialog{42, parent};
      drainGtkEvents();

      std::int32_t responseId = -1;
      dialog.signal_response().connect([&](std::int32_t id) { responseId = id; });

      auto* const titlebar = dialog.get_titlebar();
      REQUIRE(titlebar != nullptr);
      auto* const headerBar = dynamic_cast<Gtk::HeaderBar*>(titlebar);
      REQUIRE(headerBar != nullptr);

      auto* const okButton = findButtonByLabel(*headerBar, "OK");
      REQUIRE(okButton != nullptr);
      emitClicked(*okButton);
      drainGtkEvents();

      CHECK(responseId == Gtk::ResponseType::OK);
    }
  }
} // namespace ao::gtk::test
