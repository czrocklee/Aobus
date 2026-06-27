// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackCustomViewDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/entry.h>
#include <gtkmm/window.h>

#include <algorithm>

namespace ao::gtk::test
{
  TEST_CASE("TrackCustomViewDialog renders the initial custom-view draft", "[gtk][unit][track][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};

    auto spec = rt::TrackPresentationSpec{};
    spec.visibleFields = {rt::TrackField::Title};

    SECTION("dialog creation")
    {
      auto dialog = TrackCustomViewDialog{window, spec, "Initial Label"};
      drainGtkEvents();

      auto const entries = collectAll<Gtk::Entry>(dialog);
      auto const hasInitialLabel =
        std::ranges::any_of(entries, [](Gtk::Entry const* entry) { return entry->get_text() == "Initial Label"; });
      CHECK(hasInitialLabel);
    }
  }
} // namespace ao::gtk::test
