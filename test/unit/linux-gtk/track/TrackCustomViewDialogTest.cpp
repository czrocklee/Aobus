// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCustomViewDialog.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <cstdint>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    Gtk::Entry* viewNameEntry(Gtk::Widget& root)
    {
      for (auto* const entry : collectAll<Gtk::Entry>(root))
      {
        if (entry->get_placeholder_text() == "View label")
        {
          return entry;
        }
      }

      return nullptr;
    }
  } // namespace

  TEST_CASE("TrackCustomViewDialog - renders the initial custom-view draft", "[gtk][unit][track][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};

    auto spec = rt::TrackPresentationSpec{};
    spec.visibleFields = {rt::TrackField::Title};

    SECTION("dialog creation")
    {
      auto dialog = TrackCustomViewDialog{window, ao::test::englishMessageCatalog(), spec, "Initial Label"};
      drainGtkEvents();

      auto* const nameEntry = viewNameEntry(dialog);
      auto const dropdowns = collectAll<Gtk::DropDown>(dialog);
      REQUIRE(nameEntry != nullptr);
      REQUIRE(dropdowns.size() == 2);
      CHECK(nameEntry->get_text() == "Initial Label");
      CHECK(dropdowns[0]->get_selected() == 0); // Group: None
      CHECK(dropdowns[1]->get_selected() == 0); // Visible field: Title
    }

    SECTION("row tools use icon-only controls")
    {
      spec.sortBy = {{.field = rt::TrackSortField::Title, .ascending = true}};
      spec.visibleFields = {rt::TrackField::Title, rt::TrackField::Artist};

      auto dialog = TrackCustomViewDialog{window, ao::test::englishMessageCatalog(), spec, "Initial Label"};
      dialog.present();
      drainGtkEvents();

      for (auto* const button : collectAll<Gtk::Button>(dialog))
      {
        CHECK(button->get_label() != "Ascending");
        CHECK(button->get_label() != "Up");
        CHECK(button->get_label() != "Down");
        CHECK(button->get_label() != "Remove");
        CHECK(button->get_label() != "Add Sort Field");
        CHECK(button->get_label() != "Add Column");

        if (!button->get_icon_name().empty())
        {
          auto const tooltip = button->get_tooltip_text();
          REQUIRE_FALSE(tooltip.empty());
          CHECK(hasAccessibleLabel(*button, tooltip.raw()));
        }
      }
    }

    SECTION("section add actions are attached to headers")
    {
      auto dialog = TrackCustomViewDialog{window, ao::test::englishMessageCatalog(), spec, "Initial Label"};
      drainGtkEvents();

      bool foundSortAdd = false;
      bool foundColumnAdd = false;

      for (auto* const button : collectAll<Gtk::Button>(dialog))
      {
        foundSortAdd = foundSortAdd || button->get_tooltip_text() == "Add sort field";
        foundColumnAdd = foundColumnAdd || button->get_tooltip_text() == "Add column";
      }

      CHECK(foundSortAdd);
      CHECK(foundColumnAdd);
    }

    SECTION("last visible column cannot be removed")
    {
      auto dialog = TrackCustomViewDialog{window, ao::test::englishMessageCatalog(), spec, "Initial Label"};
      drainGtkEvents();

      Gtk::Button const* removeButton = nullptr;

      for (auto* const button : collectAll<Gtk::Button>(dialog))
      {
        if (button->get_tooltip_text() == "Remove")
        {
          removeButton = button;
          break;
        }
      }

      REQUIRE(removeButton != nullptr);
      CHECK_FALSE(removeButton->get_sensitive());
    }
  }

  TEST_CASE("TrackCustomViewDialog - returns only the publicly submitted draft", "[gtk][unit][track][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};
    auto spec = rt::TrackPresentationSpec{};
    spec.visibleFields = {rt::TrackField::Title};
    auto const runDialog = [](TrackCustomViewDialog& dialog)
    {
      constexpr std::uint32_t kResponseTimeoutMilliseconds = 5000;
      bool deadlineExpired = false;
      auto deadlineConnection = sigc::scoped_connection{Glib::signal_timeout().connect(
        [&]
        {
          deadlineExpired = true;
          dialog.response(Gtk::ResponseType::CANCEL);
          return false;
        },
        kResponseTimeoutMilliseconds)};
      auto optResult = dialog.runDialog();
      CHECK_FALSE(deadlineExpired);
      return optResult;
    };

    SECTION("Save returns the edited name and group with the initial visible field")
    {
      auto dialog = TrackCustomViewDialog{window, ao::test::englishMessageCatalog(), spec, "Initial Label"};
      auto* const nameEntry = viewNameEntry(dialog);
      auto const dropdowns = collectAll<Gtk::DropDown>(dialog);
      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(nameEntry != nullptr);
      REQUIRE(dropdowns.size() == 2);
      REQUIRE(saveButton != nullptr);

      auto* const groupDropdown = dropdowns.front();
      auto responseConnection = sigc::scoped_connection{Glib::signal_idle().connect(
        [nameEntry, groupDropdown, saveButton]
        {
          nameEntry->set_text("Edited View");
          groupDropdown->set_selected(2); // Album in the retained UIModel option order.
          emitClicked(*saveButton);       // Public response binding, not native pointer delivery.
          return false;
        })};

      auto const optResult = runDialog(dialog);
      REQUIRE(optResult);
      CHECK(optResult->label == "Edited View");
      CHECK_FALSE(optResult->spec.id.empty());
      CHECK(optResult->spec.groupBy == rt::TrackGroupKey::Album);
      CHECK(optResult->spec.visibleFields == std::vector{rt::TrackField::Title});
    }

    SECTION("Cancel returns no draft")
    {
      auto dialog = TrackCustomViewDialog{window, ao::test::englishMessageCatalog(), spec, "Initial Label"};
      auto* const cancelButton = findButtonByLabel(dialog, "Cancel");
      REQUIRE(cancelButton != nullptr);
      auto responseConnection = sigc::scoped_connection{Glib::signal_idle().connect(
        [cancelButton]
        {
          emitClicked(*cancelButton); // Public response binding only.
          return false;
        })};

      CHECK_FALSE(runDialog(dialog));
    }
  }

  TEST_CASE("TrackCustomViewDialog - renders locale-selected editor copy", "[gtk][unit][track][dialog][localization]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};
    auto spec = rt::TrackPresentationSpec{};
    spec.visibleFields = {rt::TrackField::Title};
    auto const textCatalog = ao::test::messageCatalog("de-DE");

    auto dialog = TrackCustomViewDialog{window, textCatalog, spec, "Meine Ansicht"};
    drainGtkEvents();

    CHECK(dialog.get_title() == "Benutzerdefinierte Ansicht bearbeiten");
    CHECK(findLabelByText(dialog, "Gruppieren nach") != nullptr);
    CHECK(findLabelByText(dialog, "Sichtbare Spalten") != nullptr);

    bool foundAddColumn = false;

    for (auto* const button : collectAll<Gtk::Button>(dialog))
    {
      foundAddColumn = foundAddColumn || button->get_tooltip_text() == "Spalte hinzufügen";
    }

    CHECK(foundAddColumn);
  }
} // namespace ao::gtk::test
