// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackSelectionController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdk.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/object.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/window.h>

#include <chrono>
#include <cstddef>
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
    auto trackId1 = kInvalidTrackId;
    auto trackId2 = kInvalidTrackId;
    auto trackId3 = kInvalidTrackId;
    auto trackId4 = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      {
        trackId1 = library::test::addTrackWithUniqueFixtureUri(
          musicLibrary, library::test::TrackSpec{.title = "Track 1", .duration = std::chrono::minutes{2}});
        trackId2 = library::test::addTrackWithUniqueFixtureUri(
          musicLibrary, library::test::TrackSpec{.title = "Track 2", .duration = std::chrono::minutes{3}});
        trackId3 = library::test::addTrackWithUniqueFixtureUri(
          musicLibrary, library::test::TrackSpec{.title = "Track 3", .duration = std::chrono::minutes{5}});
        trackId4 = library::test::addTrackWithUniqueFixtureUri(
          musicLibrary, library::test::TrackSpec{.title = "Track 4", .duration = std::chrono::minutes{4}});
      }};
    auto const& library = fixture.runtime().musicLibrary();
    auto cache = TrackRowCache{fixture.runtime().library(), ao::test::englishMessageCatalog()};

    auto modelPtr = TrackListModel::create(cache);
    auto selectionModelPtr = Gtk::MultiSelection::create(modelPtr);

    auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
    sourcePtr->addInitial(trackId1);
    sourcePtr->addInitial(trackId2);
    sourcePtr->addInitial(trackId3);
    sourcePtr->addInitial(trackId4);
    auto projectionPtr =
      std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, library);
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

      SECTION("selectedTrackIds keeps sparse GTK bitset order")
      {
        selectionModelPtr->select_item(3, false);
        selectionModelPtr->select_item(1, false);
        drainGtkEvents();

        CHECK(controller.selectedTrackCount() == 2);

        auto const ids = controller.selectedTrackIds();
        CHECK(ids == std::vector<TrackId>{trackId2, trackId4});
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

      SECTION("secondary click on blank space does not request a track menu")
      {
        controller.configureActivation();
        std::size_t requestCount = 0;
        auto subscription = controller.signalContextMenuRequested().connect([&](double, double) { ++requestCount; });
        auto host = GtkWindowFixture{};
        host.window().set_default_size(400, 400);
        host.mount(columnView);
        host.present();
        auto const secondaryClickPtr = findControllerIf<Gtk::GestureClick>(
          columnView, [](Gtk::GestureClick const& gesture) { return gesture.get_button() == GDK_BUTTON_SECONDARY; });
        REQUIRE(secondaryClickPtr);
        REQUIRE(columnView.get_height() > 1);

        ::g_signal_emit_by_name(
          secondaryClickPtr->gobj(), "released", 1, 10.0, static_cast<double>(columnView.get_height() - 1));

        CHECK(requestCount == 0);
      }

      columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
      drainGtkEvents();
    }

    drainGtkEvents();
  }
} // namespace ao::gtk::test
