// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/SmartListDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("SmartListDialog - renders the initial smart-list draft", "[gtk][unit][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library()};

    auto dialog = SmartListDialog{window, fixture.runtime(), rt::kAllTracksListId, cache};

    // Rebuild happens in idle task
    drainGtkEvents();

    CHECK(dialog.editListId() == kInvalidListId);
    CHECK(dialog.draft().kind == rt::LibraryWriter::ListKind::Smart);

    bool foundTwoPane = false;
    bool foundConfigPane = false;
    bool foundPreviewPane = false;

    for (auto* const box : collectAll<Gtk::Box>(dialog))
    {
      foundTwoPane = foundTwoPane || box->has_css_class("ao-dialog-two-pane");
      foundConfigPane = foundConfigPane || box->has_css_class("ao-dialog-config-pane");
      foundPreviewPane = foundPreviewPane || box->has_css_class("ao-dialog-preview-pane");

      if (box->has_css_class("ao-dialog-config-pane"))
      {
        CHECK_FALSE(box->get_hexpand());
      }
    }

    CHECK(foundTwoPane);
    CHECK(foundConfigPane);
    CHECK(foundPreviewPane);
  }

  TEST_CASE("SmartListDialog - invalid preview source shows the acquisition failure", "[gtk][regression][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library()};
    auto dialog = SmartListDialog{window, fixture.runtime(), ListId{999999}, cache};

    drainGtkEvents();

    bool visibleError = false;

    for (auto* const label : collectAll<Gtk::Label>(dialog))
    {
      visibleError =
        visibleError || (label->get_visible() && !label->get_text().empty() && label->has_css_class("ao-layout-error"));
    }

    CHECK(visibleError);
  }

  // The preview source is acquired from an idle task. Until it arrives the
  // dialog treats the expression as unvalidated, so the readiness pass must
  // re-run the full preview or naming a list never enables submission.
  TEST_CASE("SmartListDialog - naming a list enables submission once the preview source is ready",
            "[gtk][regression][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library()};

    auto dialog = SmartListDialog{window, fixture.runtime(), rt::kAllTracksListId, cache};

    drainGtkEvents();

    Gtk::Entry* nameEntry = nullptr;

    for (auto* const entry : collectAll<Gtk::Entry>(dialog))
    {
      if (entry->get_placeholder_text() == "List name")
      {
        nameEntry = entry;
        break;
      }
    }

    REQUIRE(nameEntry != nullptr);

    auto* const okButton = findButtonByLabel(dialog, "Create");
    REQUIRE(okButton != nullptr);
    CHECK_FALSE(okButton->get_sensitive());

    nameEntry->set_text("My List");
    drainGtkEvents();

    CHECK(okButton->get_sensitive());
  }

  // A list created from the library root, and any top-level list opened for
  // editing, carries parentId kInvalidListId. The source cache rejects that id
  // outright, so the dialog must resolve it to the All Tracks root or the
  // preview never builds and the editor opens showing an acquisition error.
  TEST_CASE("SmartListDialog - root parent previews against All Tracks", "[gtk][regression][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library()};

    auto dialog = SmartListDialog{window, fixture.runtime(), kInvalidListId, cache};

    drainGtkEvents();

    bool visibleError = false;

    for (auto* const label : collectAll<Gtk::Label>(dialog))
    {
      visibleError =
        visibleError || (label->get_visible() && !label->get_text().empty() && label->has_css_class("ao-layout-error"));
    }

    CHECK_FALSE(visibleError);

    Gtk::Entry* nameEntry = nullptr;

    for (auto* const entry : collectAll<Gtk::Entry>(dialog))
    {
      if (entry->get_placeholder_text() == "List name")
      {
        nameEntry = entry;
        break;
      }
    }

    REQUIRE(nameEntry != nullptr);

    auto* const okButton = findButtonByLabel(dialog, "Create");
    REQUIRE(okButton != nullptr);

    nameEntry->set_text("Root List");
    drainGtkEvents();

    CHECK(okButton->get_sensitive());
  }
} // namespace ao::gtk::test
