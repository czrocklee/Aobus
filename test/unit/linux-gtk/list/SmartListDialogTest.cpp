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
    window.set_child(dialog);

    // Rebuild happens in idle task
    drainGtkEvents();
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
    window.set_child(dialog);

    drainGtkEvents();
    drainGtkEvents();

    bool visibleError = false;

    for (auto* const label : collectAll<Gtk::Label>(dialog))
    {
      visibleError =
        visibleError || (label->get_visible() && !label->get_text().empty() && label->has_css_class("ao-layout-error"));
    }

    CHECK(visibleError);
  }
} // namespace ao::gtk::test
