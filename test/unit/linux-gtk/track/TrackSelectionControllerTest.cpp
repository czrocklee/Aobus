// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackSelectionController.h"

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/object.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/signallistitemfactory.h>

#include <chrono>
#include <memory>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    void appendTestColumn(Gtk::ColumnView& columnView)
    {
      auto const factoryPtr = Gtk::SignalListItemFactory::create();
      factoryPtr->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
                                         { itemPtr->set_child(*Gtk::make_managed<Gtk::Label>()); });

      factoryPtr->signal_bind().connect(
        [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
        {
          auto* const label = dynamic_cast<Gtk::Label*>(itemPtr->get_child());

          if (label != nullptr)
          {
            label->set_text("track");
          }
        });

      auto const columnPtr = Gtk::ColumnViewColumn::create("Track", factoryPtr);
      columnPtr->set_fixed_width(160);
      columnView.append_column(columnPtr);
    }
  } // namespace

  TEST_CASE("TrackSelectionController - synchronizes GTK selection with runtime views", "[gtk][unit][track][selection]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();
    auto cache = TrackRowCache{fixture.runtime().library()};

    auto modelPtr = TrackListModel::create(cache);
    auto selectionModelPtr = Gtk::MultiSelection::create(modelPtr);

    auto const trackId1 = library::test::addTrack(
      library, library::test::TrackSpec{.title = "Track 1", .duration = std::chrono::minutes{2}});
    auto const trackId2 = library::test::addTrack(
      library, library::test::TrackSpec{.title = "Track 2", .duration = std::chrono::minutes{3}});
    auto const trackId3 = library::test::addTrack(
      library, library::test::TrackSpec{.title = "Track 3", .duration = std::chrono::minutes{5}});
    auto const trackId4 = library::test::addTrack(
      library, library::test::TrackSpec{.title = "Track 4", .duration = std::chrono::minutes{4}});

    auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
    sourcePtr->addInitial(trackId1);
    sourcePtr->addInitial(trackId2);
    sourcePtr->addInitial(trackId3);
    sourcePtr->addInitial(trackId4);
    auto projectionPtr =
      std::make_shared<rt::LiveTrackListProjection>(rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, library);
    modelPtr->bindProjection(projectionPtr);
    drainGtkEvents();

    {
      auto columnView = Gtk::ColumnView{};
      appendTestColumn(columnView);
      columnView.set_model(selectionModelPtr);

      auto controller = TrackSelectionController{columnView, modelPtr, selectionModelPtr};

      SECTION("selection updates")
      {
        CHECK(controller.selectedTrackCount() == 0);

        // Select first track
        selectionModelPtr->select_item(0, true);
        drainGtkEvents();

        CHECK(controller.selectedTrackCount() == 1);
        CHECK(controller.primarySelectedTrackId() == trackId1);
        auto const ids = controller.selectedTrackIds();
        REQUIRE(ids.size() == 1);
        CHECK(ids[0] == trackId1);
      }

      SECTION("selected row helpers return rows and aggregate duration")
      {
        selectionModelPtr->select_item(0, false);
        selectionModelPtr->select_item(1, false);
        drainGtkEvents();

        auto const rows = controller.selectedRows();
        REQUIRE(rows.size() == 2);
        CHECK(rows[0]->trackId() == trackId1);
        CHECK(rows[1]->trackId() == trackId2);
        CHECK(controller.selectedTracksDuration() == std::chrono::minutes{5});

        auto const visibleIds = controller.visibleTrackIds();
        CHECK(visibleIds == std::vector<TrackId>{trackId1, trackId2, trackId3, trackId4});
      }

      SECTION("selected helpers keep sparse GTK bitset order")
      {
        selectionModelPtr->select_item(3, false);
        selectionModelPtr->select_item(1, false);
        drainGtkEvents();

        CHECK(controller.selectedTrackCount() == 2);

        auto const ids = controller.selectedTrackIds();
        CHECK(ids == std::vector<TrackId>{trackId2, trackId4});

        auto const rows = controller.selectedRows();
        REQUIRE(rows.size() == 2);
        CHECK(rows[0]->trackId() == trackId2);
        CHECK(rows[1]->trackId() == trackId4);
        CHECK(controller.selectedTracksDuration() == std::chrono::minutes{7});
      }

      SECTION("selectTrack helper")
      {
        controller.selectTrack(trackId2);
        drainGtkEvents();

        CHECK(controller.selectedTrackCount() == 1);
        CHECK(controller.primarySelectedTrackId() == trackId2);
      }

      SECTION("selectTrack keeps the requested row selected in grouped presentations")
      {
        projectionPtr->setPresentation(rt::TrackPresentationSpec{
          .groupBy = rt::TrackGroupKey::Album,
          .sortBy = {rt::TrackSortTerm{.field = rt::TrackSortField::Album},
                     rt::TrackSortTerm{.field = rt::TrackSortField::Title}},
        });
        drainGtkEvents();

        REQUIRE(projectionPtr->groupCount() == 1);
        CHECK(projectionPtr->groupAt(0).rows.start == 0);
        auto const optTrack2Index = modelPtr->indexOf(trackId2);
        REQUIRE(optTrack2Index);
        CHECK(*optTrack2Index > projectionPtr->groupAt(0).rows.start);

        auto host = GtkWindowFixture{};
        host.mount(columnView);
        host.present();
        REQUIRE(columnView.get_mapped());

        controller.selectTrack(trackId2);
        drainGtkEvents();

        CHECK(controller.selectedTrackCount() == 1);
        CHECK(controller.primarySelectedTrackId() == trackId2);
      }

      SECTION("signal propagation")
      {
        bool changed = false;
        controller.signalSelectionChanged().connect([&] { changed = true; });

        selectionModelPtr->select_item(1, true);
        drainGtkEvents();

        CHECK(changed == true);
      }

      columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
      drainGtkEvents();
    }

    drainGtkEvents();
  }
} // namespace ao::gtk::test
