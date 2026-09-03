// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/adjustment.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

    uimodel::TrackColumnState const* stateForField(std::vector<uimodel::TrackColumnState> const& layout,
                                                   rt::TrackField field)
    {
      auto const it = std::ranges::find(layout, field, &uimodel::TrackColumnState::field);
      return it == layout.end() ? nullptr : &*it;
    }
  } // namespace

  TEST_CASE("TrackColumnController - builds and updates visible track columns", "[gtk][unit][track][column]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor);
    auto columnLayouts = uimodel::TrackColumnLayouts{changes};

    auto columnView = Gtk::ColumnView{};
    auto controller =
      TrackColumnController{columnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};

    SECTION("configureColumns creates all supported columns")
    {
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto const columnsPtr = columnView.get_columns();
      CHECK(columnsPtr->get_n_items() > 0);
    }

    SECTION("applyColumnLayout updates visibility")
    {
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto visible = std::vector{rt::TrackField::Title, rt::TrackField::Artist};
      controller.applyColumnLayout(visible);

      auto const titleColumnPtr = columnForField(columnView, rt::TrackField::Title);
      auto const artistColumnPtr = columnForField(columnView, rt::TrackField::Artist);
      auto const albumColumnPtr = columnForField(columnView, rt::TrackField::Album);
      REQUIRE(titleColumnPtr);
      REQUIRE(artistColumnPtr);
      REQUIRE(albumColumnPtr);

      CHECK(titleColumnPtr->get_visible());
      CHECK(artistColumnPtr->get_visible());
      CHECK_FALSE(albumColumnPtr->get_visible());
      CHECK_FALSE(titleColumnPtr->get_expand());
      CHECK_FALSE(artistColumnPtr->get_expand());
    }

    SECTION("setup and sync do not persist initial column construction")
    {
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto visible = std::vector{rt::TrackField::Title, rt::TrackField::Artist};
      controller.syncLayout(visible);
      drainGtkEvents();

      CHECK(columnLayouts.snapshot().empty());
    }

    SECTION("a leading utility column stays fixed outside the persisted field layout")
    {
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });
      auto const utilityPtr = Gtk::ColumnViewColumn::create("", Gtk::SignalListItemFactory::create());
      utilityPtr->set_id("test-utility");
      utilityPtr->set_fixed_width(36);
      controller.prependUtilityColumn(utilityPtr);
      controller.syncLayout(std::vector{rt::TrackField::Artist, rt::TrackField::Title});

      auto const columnsPtr = columnView.get_columns();
      REQUIRE(columnsPtr);
      CHECK(columnsPtr->get_object(0) == utilityPtr);
      CHECK(fieldForColumn(std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(1))) ==
            rt::TrackField::Artist);
      CHECK(fieldForColumn(std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(2))) ==
            rt::TrackField::Title);
      drainGtkEvents();
      CHECK(columnLayouts.snapshot().empty());
    }

    SECTION("stored hidden columns remain hidden and survive a visible-column resize")
    {
      columnLayouts.restore(
        {{rt::kAllTracksListId,
          {
            uimodel::TrackColumnState{.field = rt::TrackField::Album, .weight = 1.0, .visible = false},
          }}},
        {});
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });
      controller.syncLayout(std::vector{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album});

      auto const albumColumnPtr = columnForField(columnView, rt::TrackField::Album);
      auto const titleColumnPtr = columnForField(columnView, rt::TrackField::Title);
      REQUIRE(albumColumnPtr);
      REQUIRE(titleColumnPtr);
      CHECK_FALSE(albumColumnPtr->get_visible());

      titleColumnPtr->set_fixed_width(333);
      drainGtkEvents();

      auto const& stored = columnLayouts.layoutForList(rt::kAllTracksListId);
      auto const* albumState = stateForField(stored, rt::TrackField::Album);
      REQUIRE(albumState != nullptr);
      CHECK_FALSE(albumState->visible);
    }

    SECTION("column layout writes do not bounce between open views")
    {
      auto events = std::vector<ListId>{};
      auto sub = columnLayouts.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });

      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto secondColumnView = Gtk::ColumnView{};
      auto secondController =
        TrackColumnController{secondColumnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};
      secondController.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto firstVisible = std::vector{rt::TrackField::Title, rt::TrackField::Artist};
      auto secondVisible = std::vector{rt::TrackField::Title, rt::TrackField::Album};
      controller.syncLayout(firstVisible);
      secondController.syncLayout(secondVisible);
      drainGtkEvents();

      CHECK(events.empty());

      auto const titleColumnPtr = columnForField(columnView, rt::TrackField::Title);
      REQUIRE(titleColumnPtr);

      titleColumnPtr->set_fixed_width(333);
      drainGtkEvents();

      REQUIRE(events.size() == 1);
      CHECK(events[0] == rt::kAllTracksListId);

      auto const& stored = columnLayouts.layoutForList(rt::kAllTracksListId);
      CHECK(std::ranges::contains(stored, rt::TrackField::Artist, &uimodel::TrackColumnState::field));
      CHECK_FALSE(std::ranges::contains(stored, rt::TrackField::Album, &uimodel::TrackColumnState::field));
      auto const* titleState = stateForField(stored, rt::TrackField::Title);
      REQUIRE(titleState != nullptr);
      CHECK(titleState->width == -1);
      CHECK(titleState->weight > 0.0);

      drainGtkEvents();
      CHECK(events.size() == 1);
    }

    SECTION("title position CSS updates are coalesced through idle")
    {
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto visible = std::vector{rt::TrackField::Artist, rt::TrackField::Title};
      controller.syncLayout(visible);
      auto const initialCss = controller.titlePositionCss();
      REQUIRE_FALSE(initialCss.empty());

      auto const artistColumnPtr = columnForField(columnView, rt::TrackField::Artist);
      REQUIRE(artistColumnPtr);

      artistColumnPtr->set_fixed_width(320);
      artistColumnPtr->set_fixed_width(420);

      CHECK(controller.isTitlePositionUpdateQueued());

      drainGtkEvents();

      CHECK_FALSE(controller.isTitlePositionUpdateQueued());
      CHECK(controller.titlePositionCss() != initialCss);
    }

    SECTION("allocated column view exposes the viewport as horizontal page size")
    {
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      auto visible = std::vector{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Duration};
      controller.syncLayout(visible);
      auto host = GtkWindowFixture{};
      host.window().set_default_size(640, 240);
      host.mount(columnView);
      host.present();

      auto const adjPtr = columnView.get_hadjustment();
      REQUIRE(adjPtr);
      CHECK(static_cast<std::int32_t>(std::lround(adjPtr->get_page_size())) == columnView.get_width());
    }
  }

  TEST_CASE("TrackColumnController - follows the active horizontal adjustment",
            "[gtk][regression][track-column][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor);
    auto columnLayouts = uimodel::TrackColumnLayouts{changes};
    auto columnView = Gtk::ColumnView{};
    auto controller =
      TrackColumnController{columnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};
    controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });
    controller.syncLayout(std::vector{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Duration});

    auto const viewportAdjustmentPtr = Gtk::Adjustment::create(0.0, 0.0, 1000.0, 1.0, 10.0, 640.0);
    columnView.set_hadjustment(viewportAdjustmentPtr);
    drainGtkEvents();

    auto const titleColumnPtr = columnForField(columnView, rt::TrackField::Title);
    auto const artistColumnPtr = columnForField(columnView, rt::TrackField::Artist);
    auto const durationColumnPtr = columnForField(columnView, rt::TrackField::Duration);
    REQUIRE(titleColumnPtr);
    REQUIRE(artistColumnPtr);
    REQUIRE(durationColumnPtr);

    CHECK(titleColumnPtr->get_fixed_width() + artistColumnPtr->get_fixed_width() +
            durationColumnPtr->get_fixed_width() ==
          640);

    viewportAdjustmentPtr->set_page_size(720.0);
    drainGtkEvents();

    CHECK(titleColumnPtr->get_fixed_width() + artistColumnPtr->get_fixed_width() +
            durationColumnPtr->get_fixed_width() ==
          720);
  }
} // namespace ao::gtk::test
