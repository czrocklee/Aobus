// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnController.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/presentation/TrackColumnDefaults.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/adjustment.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <array>
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

  TEST_CASE("TrackColumnController - teardown cancels queued updates while the column view survives")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor);
    auto columnLayouts = uimodel::TrackColumnLayouts{changes};
    auto columnView = Gtk::ColumnView{};
    auto titleColumnPtr = Glib::RefPtr<Gtk::ColumnViewColumn>{};

    {
      auto controller =
        TrackColumnController{columnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });
      controller.applyColumnLayout(std::array{rt::TrackField::Title, rt::TrackField::Artist});
      drainGtkEvents();
      REQUIRE(columnLayouts.snapshot().empty());
      titleColumnPtr = columnForField(columnView, rt::TrackField::Title);
      REQUIRE(titleColumnPtr);

      titleColumnPtr->set_fixed_width(333);
      REQUIRE(controller.isTitlePositionUpdateQueued());
    }

    drainGtkEvents();
    CHECK(columnLayouts.snapshot().empty());

    // The surviving emitter must not enqueue work for its former controller.
    titleColumnPtr->set_fixed_width(444);
    drainGtkEvents();
    CHECK(columnLayouts.snapshot().empty());
  }

  TEST_CASE("TrackColumnController - configures registered fields with their titles and factories")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor);
    auto columnLayouts = uimodel::TrackColumnLayouts{changes};

    auto columnView = Gtk::ColumnView{};
    auto controller =
      TrackColumnController{columnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};

    auto configuredFields = std::vector<rt::TrackField>{};
    auto factories = std::vector<Glib::RefPtr<Gtk::SignalListItemFactory>>{};
    controller.configureColumns(
      [&configuredFields, &factories](rt::TrackField field)
      {
        configuredFields.push_back(field);
        factories.push_back(Gtk::SignalListItemFactory::create());
        return factories.back();
      });
    auto expectedFields = std::vector<rt::TrackField>{};

    for (auto const& definition : rt::trackFieldDefinitions())
    {
      if (definition.presentable)
      {
        expectedFields.push_back(definition.field);
      }
    }

    REQUIRE(configuredFields == expectedFields);
    REQUIRE(factories.size() == expectedFields.size());
    auto const columnsPtr = columnView.get_columns();
    REQUIRE(columnsPtr);
    REQUIRE(columnsPtr->get_n_items() == expectedFields.size());

    for (::guint index = 0; index < columnsPtr->get_n_items(); ++index)
    {
      auto const columnPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(index));
      REQUIRE(columnPtr);
      auto const field = expectedFields[index];
      CHECK(fieldForColumn(columnPtr) == field);
      CHECK(columnPtr->get_factory() == factories[index]);
      CHECK(columnPtr->get_id().raw() == rt::trackFieldId(field));
      CHECK(columnPtr->get_title().raw() == uimodel::trackFieldLabel(ao::test::englishMessageCatalog(), field));
      CHECK(columnPtr->get_resizable());
      CHECK(columnPtr->get_fixed_width() == uimodel::trackColumnDefaults(field).width);
    }

    auto const titleColumnPtr = columnForField(columnView, rt::TrackField::Title);
    REQUIRE(titleColumnPtr);
    CHECK(titleColumnPtr->get_title() == "Title");
  }

  TEST_CASE("TrackColumnController - applies and persists field layouts")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor);
    auto columnLayouts = uimodel::TrackColumnLayouts{changes};

    auto columnView = Gtk::ColumnView{};
    auto controller =
      TrackColumnController{columnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};

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

    SECTION("setting up and applying a layout does not persist initial column construction")
    {
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

      controller.applyColumnLayout(std::array{rt::TrackField::Title, rt::TrackField::Artist});
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
      controller.applyColumnLayout(std::array{rt::TrackField::Artist, rt::TrackField::Title});

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

    SECTION("an unpositioned override follows authored order until a native resize")
    {
      auto const yearOverride =
        uimodel::TrackColumnState{.field = rt::TrackField::Year, .visibility = uimodel::TrackColumnVisibility::Show};
      REQUIRE_FALSE(yearOverride.hasPosition());
      auto const hiddenAlbum = uimodel::TrackColumnState{
        .field = rt::TrackField::Album, .weight = 1.75, .visibility = uimodel::TrackColumnVisibility::Hide};
      auto const seeded = std::vector{yearOverride, hiddenAlbum};
      columnLayouts.restore({{rt::kAllTracksListId, seeded}}, {});
      std::int32_t changeCount = 0;
      auto const subscription = columnLayouts.signalChanged().connect([&changeCount](ListId) { ++changeCount; });
      auto const presentation = rt::TrackPresentationSpec{
        .visibleFields = {rt::TrackField::Title, rt::TrackField::Duration, rt::TrackField::Year}};
      auto const fields = uimodel::visibleTrackFieldsInStoredLayout(presentation, seeded);
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });
      controller.applyColumnLayout(fields);

      auto const titleColumnPtr = columnForField(columnView, rt::TrackField::Title);
      auto const durationColumnPtr = columnForField(columnView, rt::TrackField::Duration);
      auto const yearColumnPtr = columnForField(columnView, rt::TrackField::Year);
      auto const columnsPtr = columnView.get_columns();
      REQUIRE(titleColumnPtr);
      REQUIRE(durationColumnPtr);
      REQUIRE(yearColumnPtr);
      REQUIRE(columnsPtr);
      CHECK(columnsPtr->get_object(0) == titleColumnPtr);
      CHECK(columnsPtr->get_object(1) == durationColumnPtr);
      CHECK(columnsPtr->get_object(2) == yearColumnPtr);
      CHECK(yearColumnPtr->get_visible());

      auto const adjustmentPtr = Gtk::Adjustment::create(0.0, 0.0, 1000.0, 1.0, 10.0, 640.0);
      columnView.set_hadjustment(adjustmentPtr);
      drainGtkEvents();
      auto const titleWidth = titleColumnPtr->get_fixed_width();
      adjustmentPtr->set_page_size(720.0);
      drainGtkEvents();
      CHECK(titleColumnPtr->get_fixed_width() > titleWidth);
      CHECK(yearColumnPtr->get_fixed_width() == uimodel::trackColumnDefaults(rt::TrackField::Year).width);
      CHECK(columnLayouts.layoutForList(rt::kAllTracksListId) == seeded);
      CHECK(changeCount == 0);

      yearColumnPtr->set_fixed_width(133);
      drainGtkEvents();
      auto const& stored = columnLayouts.layoutForList(rt::kAllTracksListId);
      auto const* const yearState = stateForField(stored, rt::TrackField::Year);
      auto const* const albumState = stateForField(stored, rt::TrackField::Album);
      REQUIRE(yearState);
      REQUIRE(albumState);
      CHECK(yearState->hasPosition());
      CHECK(*yearState == uimodel::TrackColumnState{.field = rt::TrackField::Year,
                                                    .width = 133,
                                                    .visibility = uimodel::TrackColumnVisibility::Show});
      CHECK(*albumState == hiddenAlbum);
      CHECK(uimodel::visibleTrackFieldsInStoredLayout(presentation, stored) == fields);
      CHECK(changeCount == 1);
    }

    SECTION("a native fixed-width resize writes pixels without moving or resizing a hidden column")
    {
      auto const hiddenAlbum = uimodel::TrackColumnState{
        .field = rt::TrackField::Album, .weight = 1.75, .visibility = uimodel::TrackColumnVisibility::Hide};
      auto const seeded = std::vector{
        uimodel::TrackColumnState{.field = rt::TrackField::Title, .weight = 3.0},
        hiddenAlbum,
        uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 96},
      };
      columnLayouts.restore({{rt::kAllTracksListId, seeded}}, {});
      controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });
      controller.applyColumnLayout(std::array{rt::TrackField::Title, rt::TrackField::Duration});

      auto const albumColumnPtr = columnForField(columnView, rt::TrackField::Album);
      auto const durationColumnPtr = columnForField(columnView, rt::TrackField::Duration);
      REQUIRE(albumColumnPtr);
      REQUIRE(durationColumnPtr);
      CHECK_FALSE(albumColumnPtr->get_visible());

      durationColumnPtr->set_fixed_width(133);
      drainGtkEvents();

      auto const& stored = columnLayouts.layoutForList(rt::kAllTracksListId);
      REQUIRE(stored.size() == 3);
      CHECK(stored[1] == hiddenAlbum);
      CHECK(stored[2] == uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 133});
    }
  }

  TEST_CASE("TrackColumnController - publishes one layout change across open views")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor);
    auto columnLayouts = uimodel::TrackColumnLayouts{changes};

    auto columnView = Gtk::ColumnView{};
    auto controller =
      TrackColumnController{columnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};

    auto events = std::vector<ListId>{};
    auto sub = columnLayouts.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });

    controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

    auto secondColumnView = Gtk::ColumnView{};
    auto secondController =
      TrackColumnController{secondColumnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};
    secondController.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

    controller.applyColumnLayout(std::array{rt::TrackField::Title, rt::TrackField::Artist});
    secondController.applyColumnLayout(std::array{rt::TrackField::Title, rt::TrackField::Album});
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

  TEST_CASE("TrackColumnController - coalesces title position CSS updates through idle")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor);
    auto columnLayouts = uimodel::TrackColumnLayouts{changes};

    auto columnView = Gtk::ColumnView{};
    auto controller =
      TrackColumnController{columnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};

    controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

    controller.applyColumnLayout(std::array{rt::TrackField::Artist, rt::TrackField::Title});
    drainGtkEvents();
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
    CHECK(controller.titlePositionCss() == "columnview { --ao-title-x: 420.0px; }");
  }

  TEST_CASE("TrackColumnController - exposes the realized viewport as horizontal page size")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor);
    auto columnLayouts = uimodel::TrackColumnLayouts{changes};

    auto columnView = Gtk::ColumnView{};
    auto controller =
      TrackColumnController{columnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};

    controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });

    controller.applyColumnLayout(std::array{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Duration});
    auto host = GtkWindowFixture{};
    host.window().set_default_size(640, 240);
    host.mount(columnView);
    host.present();

    auto const adjPtr = columnView.get_hadjustment();
    REQUIRE(adjPtr);
    CHECK(static_cast<std::int32_t>(std::lround(adjPtr->get_page_size())) == columnView.get_width());
  }

  TEST_CASE("TrackColumnController - follows the active horizontal adjustment")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor);
    auto columnLayouts = uimodel::TrackColumnLayouts{changes};
    auto columnView = Gtk::ColumnView{};
    auto controller =
      TrackColumnController{columnView, columnLayouts, ao::test::englishMessageCatalog(), rt::kAllTracksListId};
    controller.configureColumns([](rt::TrackField) { return Gtk::SignalListItemFactory::create(); });
    controller.applyColumnLayout(std::array{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Duration});

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
