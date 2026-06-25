// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "../../TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::test
{
  namespace
  {
    void realizeColumnView(Gtk::Window& window, Gtk::ColumnView& columnView)
    {
      window.set_default_size(400, 200);
      window.set_visible(true);
      drainGtkEvents();

      std::int32_t minimum = std::int32_t{};
      std::int32_t natural = std::int32_t{};
      std::int32_t minimumBaseline = std::int32_t{};
      std::int32_t naturalBaseline = std::int32_t{};
      columnView.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
      columnView.measure(Gtk::Orientation::VERTICAL, 400, minimum, natural, minimumBaseline, naturalBaseline);
      columnView.size_allocate(Gtk::Allocation{0, 0, 400, 200}, -1);
      drainGtkEvents();
    }
  } // namespace

  TEST_CASE("TrackColumnFactoryBuilder - factory lifecycle", "[gtk][track][column]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();
    auto cache = TrackRowCache{fixture.runtime().library()};

    {
      auto window = Gtk::Window{};
      auto columnView = Gtk::ColumnView{};
      window.set_child(columnView);

      auto trackId = TrackId{kInvalidTrackId};
      {
        auto txn = library.writeTransaction();
        auto writer = library.tracks().writer(txn);
        auto builder = library::TrackBuilder::createNew();
        builder.metadata().title("Test Title").artist("Test Artist");
        builder.property().duration(std::chrono::minutes{2});
        auto serializeResult = builder.serialize(txn, library.dictionary(), library.resources());
        REQUIRE(serializeResult);
        auto const [hot, cold] = *serializeResult;
        trackId = ao::test::requireValue(writer.createHotCold(hot, cold)).first;
        txn.commit();
      }

      auto sourcePtr = std::make_shared<MutableTrackSource>();
      sourcePtr->addInitial(trackId);
      auto projectionPtr = std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, *sourcePtr, library);
      auto modelPtr = TrackListModel::create(cache);
      modelPtr->bindProjection(projectionPtr);

      auto selectionPtr = Gtk::SingleSelection::create(modelPtr);
      columnView.set_model(selectionPtr);

      SECTION("static column (e.g. Duration)")
      {
        auto factoryPtr = buildColumnFactory(rt::TrackField::Duration, [](auto, auto, auto) {}, *modelPtr);
        auto columnPtr = Gtk::ColumnViewColumn::create("Duration", factoryPtr);
        columnView.append_column(columnPtr);

        realizeColumnView(window, columnView);

        auto* const label = findLabelByText(columnView, "2:00");
        REQUIRE(label != nullptr);
        CHECK(label->get_single_line_mode());
        CHECK(label->get_lines() == 1);

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("editable column (e.g. Title)")
      {
        auto committedValue = std::string{};
        auto factoryPtr =
          buildColumnFactory(rt::TrackField::Title, [&](auto, auto, auto val) { committedValue = val; }, *modelPtr);
        auto columnPtr = Gtk::ColumnViewColumn::create("Title", factoryPtr);
        columnView.append_column(columnPtr);

        drainGtkEvents();

        auto rowPtr = cache.trackRow(trackId);
        REQUIRE(rowPtr);

        // Realize the cell, then drive an inline edit through the commit handler
        // that is now wired once at setup (not per bind). The slot must resolve
        // the currently-bound row from the ListItem and invoke commitFn with the
        // typed value, then collapse the editor back to its display child.
        realizeColumnView(window, columnView);

        auto* const label = findLabelByText(columnView, "Test Title");
        REQUIRE(label != nullptr);
        CHECK(label->get_single_line_mode());
        CHECK(label->get_lines() == 1);

        // Drive the now-playing highlight through the model signal (the path used
        // in production); the per-cell subscription must restyle the realized row.
        CHECK(findWidgetByClass<Gtk::Widget>(columnView, "ao-playing-row") == nullptr);

        modelPtr->setPlayingTrackId(trackId);
        drainGtkEvents();
        REQUIRE(findWidgetByClass<Gtk::Widget>(columnView, "ao-playing-row") != nullptr);

        modelPtr->setPlayingTrackId(kInvalidTrackId);
        drainGtkEvents();
        CHECK(findWidgetByClass<Gtk::Widget>(columnView, "ao-playing-row") == nullptr);

        auto* const entry = findWidget<Gtk::Entry>(columnView);
        REQUIRE(entry != nullptr);

        entry->set_text("Edited Title");
        ::g_signal_emit_by_name(entry->gobj(), "activate");
        drainGtkEvents();

        CHECK(committedValue == "Edited Title");

        auto* const stack = findWidget<Gtk::Stack>(columnView);
        REQUIRE(stack != nullptr);
        CHECK(stack->get_visible_child_name() == "display");

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("bind all fields")
      {
        for (auto const& def : rt::trackFieldDefinitions())
        {
          auto factoryPtr = buildColumnFactory(def.field, [](auto, auto, auto) {}, *modelPtr);
          auto columnPtr = Gtk::ColumnViewColumn::create(std::string{def.label}, factoryPtr);
          columnView.append_column(columnPtr);
        }

        drainGtkEvents();

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      window.unset_child();
      drainGtkEvents();
    }

    drainGtkEvents();
  }
} // namespace ao::gtk::test
