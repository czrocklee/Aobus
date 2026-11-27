// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCustomViewDialog.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/stringobject.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <variant>
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

    std::string selectedSortLabel(Gtk::DropDown& dropdown)
    {
      auto const itemPtr = std::dynamic_pointer_cast<Gtk::StringObject>(dropdown.get_selected_item());
      return itemPtr ? itemPtr->get_string().raw() : std::string{};
    }
  } // namespace

  TEST_CASE("TrackCustomViewDialog - renders the initial custom-view draft")
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

  TEST_CASE("TrackCustomViewDialog - collects the edited draft")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};
    auto spec = rt::TrackPresentationSpec{};
    spec.visibleFields = {rt::TrackField::Title};
    auto dialog = TrackCustomViewDialog{window, ao::test::englishMessageCatalog(), spec, "Initial Label"};

    auto* const nameEntry = viewNameEntry(dialog);
    auto const dropdowns = collectAll<Gtk::DropDown>(dialog);
    REQUIRE(nameEntry != nullptr);
    REQUIRE(dropdowns.size() == 2);

    nameEntry->set_text("Edited View");
    dropdowns.front()->set_selected(2); // Album in the retained UIModel option order.

    auto const draft = dialog.collectState();
    CHECK(draft.label == "Edited View");
    CHECK(std::holds_alternative<rt::CustomPresetId>(draft.spec.presetId));
    CHECK(draft.spec.groupBy == rt::TrackGroupKey::Album);
    CHECK(draft.spec.visibleFields == std::vector{rt::TrackField::Title});
  }

  TEST_CASE("TrackCustomViewDialog - keeps the sort choice in sync after duplicate rejection and rebuild")
  {
    using i18n::MessageId;
    using i18n::requiredText;
    using rt::TrackSortField;

    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};
    auto const catalog = ao::test::messageCatalog("de-DE");
    auto const& spec = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::ClassicalWorks).spec;
    auto const sortIndex = [&spec](TrackSortField field)
    {
      auto const term = std::ranges::find(spec.sortBy, field, &rt::TrackSortTerm::field);
      REQUIRE(term != spec.sortBy.end());
      return static_cast<std::size_t>(term - spec.sortBy.begin());
    };
    auto const recordingYearIndex = sortIndex(TrackSortField::RecordingYear);
    auto const workIndex = sortIndex(TrackSortField::Work);
    auto const recordingYearLabel = requiredText(catalog, MessageId::TrackSortRecordingYear);
    auto const dropdownCount = 1U + spec.sortBy.size() + spec.visibleFields.size();
    auto dialog = TrackCustomViewDialog{window, catalog, spec, "Classical view"};

    {
      auto const dropdowns = collectAll<Gtk::DropDown>(dialog);
      REQUIRE(dropdowns.size() == dropdownCount);
      auto& recordingYear = *dropdowns[1U + recordingYearIndex]; // Skip the group selector.
      auto& work = *dropdowns[1U + workIndex];
      CHECK(selectedSortLabel(recordingYear) == recordingYearLabel);
      REQUIRE(selectedSortLabel(work) == requiredText(catalog, MessageId::TrackFieldWork));
      CHECK(dialog.collectState().spec.sortBy == spec.sortBy);

      recordingYear.set_selected(work.get_selected());
      CHECK(selectedSortLabel(recordingYear) == recordingYearLabel);
      CHECK(dialog.collectState().spec.sortBy == spec.sortBy);
    }

    {
      auto const buttons = collectAll<Gtk::Button>(dialog);
      auto const addLabel = requiredText(catalog, MessageId::GtkCustomViewAddSortField);
      auto const addButton = std::ranges::find_if(
        buttons, [addLabel](auto const* button) { return button->get_tooltip_text().raw() == addLabel; });
      REQUIRE(addButton != buttons.end());
      emitClicked(**addButton);
    }

    // Borrow the RecordingYear row again from the replacement list.
    auto const dropdowns = collectAll<Gtk::DropDown>(dialog);
    REQUIRE(dropdowns.size() == dropdownCount + 1U);
    auto& recordingYear = *dropdowns[1U + recordingYearIndex];
    auto expectedSort = dialog.collectState().spec.sortBy;
    REQUIRE(expectedSort.size() == spec.sortBy.size() + 1U);
    CHECK(std::equal(spec.sortBy.begin(), spec.sortBy.end(), expectedSort.begin()));
    CHECK(selectedSortLabel(recordingYear) == recordingYearLabel);

    auto const optionsPtr = std::dynamic_pointer_cast<Gtk::StringList>(recordingYear.get_model());
    REQUIRE(optionsPtr != nullptr);
    auto const recordingDateLabel = requiredText(catalog, MessageId::TrackFieldRecordingDate);
    guint index = 0;

    while (index < optionsPtr->get_n_items() && optionsPtr->get_string(index).raw() != recordingDateLabel)
    {
      ++index;
    }

    REQUIRE(index < optionsPtr->get_n_items());
    recordingYear.set_selected(index);
    expectedSort[recordingYearIndex].field = TrackSortField::RecordingDate;
    CHECK(selectedSortLabel(recordingYear) == recordingDateLabel);
    CHECK(dialog.collectState().spec.sortBy == expectedSort);
  }

  TEST_CASE("TrackCustomViewDialog - renders locale-selected editor copy")
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
