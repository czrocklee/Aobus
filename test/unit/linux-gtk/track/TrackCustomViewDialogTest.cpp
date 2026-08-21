// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCustomViewDialog.h"

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("TrackCustomViewDialog - renders the initial custom-view draft", "[gtk][unit][track][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};

    auto spec = rt::TrackPresentationSpec{};
    spec.visibleFields = {rt::TrackField::Title};

    SECTION("dialog creation")
    {
      auto dialog = TrackCustomViewDialog{window, ao::test::englishPresentationTextCatalog(), spec, "Initial Label"};
      drainGtkEvents();

      auto const entries = collectAll<Gtk::Entry>(dialog);
      CHECK_FALSE(entries.empty());
    }

    SECTION("row tools use icon-only controls")
    {
      spec.sortBy = {{.field = rt::TrackSortField::Title, .ascending = true}};
      spec.visibleFields = {rt::TrackField::Title, rt::TrackField::Artist};

      auto dialog = TrackCustomViewDialog{window, ao::test::englishPresentationTextCatalog(), spec, "Initial Label"};
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
      auto dialog = TrackCustomViewDialog{window, ao::test::englishPresentationTextCatalog(), spec, "Initial Label"};
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
      auto dialog = TrackCustomViewDialog{window, ao::test::englishPresentationTextCatalog(), spec, "Initial Label"};
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

  TEST_CASE("TrackCustomViewDialog - renders locale-selected editor copy", "[gtk][unit][localization]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};
    auto spec = rt::TrackPresentationSpec{};
    spec.visibleFields = {rt::TrackField::Title};
    auto const textCatalog = ao::test::presentationTextCatalog("de-DE");

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
