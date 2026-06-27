// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/signallistitemfactory.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    std::optional<rt::TrackField> fieldForColumn(Glib::RefPtr<Gtk::ColumnViewColumn> const& columnPtr)
    {
      if (!columnPtr)
      {
        return std::nullopt;
      }

      return rt::trackFieldFromId(std::string{columnPtr->get_id()});
    }

    Glib::RefPtr<Gtk::ColumnViewColumn> columnForField(Gtk::ColumnView& columnView, rt::TrackField field)
    {
      auto const columnsPtr = columnView.get_columns();

      if (!columnsPtr)
      {
        return {};
      }

      for (guint i = 0; i < columnsPtr->get_n_items(); ++i)
      {
        auto columnPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(i));

        if (fieldForColumn(columnPtr) == field)
        {
          return columnPtr;
        }
      }

      return {};
    }
  } // namespace

  TEST_CASE("TrackColumnController builds and updates visible track columns", "[gtk][unit][track][column]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto presentationStore = uimodel::track::TrackPresentationViewModel{runtime.workspace()};

    auto columnView = Gtk::ColumnView{};
    auto controller = TrackColumnController{columnView, presentationStore, rt::kAllTracksListId};

    SECTION("setupColumns creates all supported columns")
    {
      controller.setupColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto const columnsPtr = columnView.get_columns();
      CHECK(columnsPtr->get_n_items() > 0);
    }

    SECTION("applyColumnLayout updates visibility")
    {
      controller.setupColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto visible = std::vector{rt::TrackField::Title, rt::TrackField::Artist};
      controller.applyColumnLayout(visible);

      auto const columnsPtr = columnView.get_columns();
      REQUIRE(columnsPtr);
      REQUIRE(columnsPtr->get_n_items() >= visible.size());

      auto const titleColumnPtr = columnForField(columnView, rt::TrackField::Title);
      auto const artistColumnPtr = columnForField(columnView, rt::TrackField::Artist);
      auto const albumColumnPtr = columnForField(columnView, rt::TrackField::Album);
      REQUIRE(titleColumnPtr);
      REQUIRE(artistColumnPtr);
      REQUIRE(albumColumnPtr);

      CHECK(std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(0)) == titleColumnPtr);
      CHECK(std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(1)) == artistColumnPtr);
      CHECK(titleColumnPtr->get_visible());
      CHECK(artistColumnPtr->get_visible());
      CHECK_FALSE(albumColumnPtr->get_visible());
      CHECK(titleColumnPtr->get_expand());
      CHECK_FALSE(artistColumnPtr->get_expand());

      for (guint i = 0; i < columnsPtr->get_n_items(); ++i)
      {
        auto colPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(i));

        if (auto const optField = fieldForColumn(colPtr);
            optField && *optField != rt::TrackField::Title && *optField != rt::TrackField::Artist)
        {
          CHECK_FALSE(colPtr->get_visible());
        }
      }
    }
  }
} // namespace ao::gtk::test
