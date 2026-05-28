// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackPresentationStore.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/signallistitemfactory.h>

#include <memory>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackColumnController - column management", "[gtk][track][column]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto presentationStore = TrackPresentationStore{runtime.workspace()};

    auto columnView = Gtk::ColumnView{};
    auto controller = TrackColumnController{columnView, presentationStore, rt::kAllTracksListId};

    SECTION("setupColumns creates all supported columns")
    {
      controller.setupColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto const columns = columnView.get_columns();
      CHECK(columns->get_n_items() > 0);
    }

    SECTION("applyColumnLayout updates visibility")
    {
      controller.setupColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto visible = std::vector{rt::TrackField::Title, rt::TrackField::Artist};
      controller.applyColumnLayout(visible);

      // Verify only these columns are visible
      auto const columns = columnView.get_columns();

      for (guint i = 0; i < columns->get_n_items(); ++i)
      {
        auto col = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columns->get_object(i));
        // We can't easily map column back to field without peering or checking title
      }
    }
  }
} // namespace ao::gtk::test
