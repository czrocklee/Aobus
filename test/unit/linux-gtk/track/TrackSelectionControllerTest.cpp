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
#include "track/TrackRowBinding.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdk.h>
#include <glib-object.h>
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
#include <sigc++/scoped_connection.h>

#include <algorithm>
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
          auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr->get_item());

          if (label != nullptr && rowPtr != nullptr)
          {
            label->set_text("track");
            ::g_object_set_data(G_OBJECT(label->gobj()),
                                kBoundTrackIdDataKey,
                                GUINT_TO_POINTER(static_cast<guint>(rowPtr->trackId().raw())));
          }
        });

      factoryPtr->signal_unbind().connect(
        [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
        {
          if (auto* const child = itemPtr->get_child(); child != nullptr)
          {
            ::g_object_set_data(G_OBJECT(child->gobj()), kBoundTrackIdDataKey, nullptr);
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
    auto cache = TrackRowCache{fixture.runtime().library(), ao::test::englishMessageCatalog()};

    auto modelPtr = TrackListModel::create(cache);
    auto selectionModelPtr = Gtk::MultiSelection::create(modelPtr);

    auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
    sourcePtr->addInitial(trackId1);
    sourcePtr->addInitial(trackId2);
    sourcePtr->addInitial(trackId3);
    sourcePtr->addInitial(trackId4);
    auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
      fixture.runtime().views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
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
        auto subscription =
          sigc::scoped_connection{controller.signalSelectionChanged().connect([&] { changed = true; })};

        selectionModelPtr->select_item(1, true);
        drainGtkEvents();

        CHECK(changed == true);
      }

      SECTION("secondary click reports the exact picked track and coordinates")
      {
        controller.configureActivation();
        std::size_t requestCount = 0;
        double requestedX = 0.0;
        double requestedY = 0.0;
        auto subscription = sigc::scoped_connection{controller.signalContextMenuRequested().connect(
          [&](double const xPosition, double const yPosition)
          {
            ++requestCount;
            requestedX = xPosition;
            requestedY = yPosition;
          })};
        auto host = GtkWindowFixture{};
        host.window().set_default_size(400, 400);
        host.mount(columnView);
        host.present();
        auto const secondaryClickPtr = findControllerIf<Gtk::GestureClick>(
          columnView, [](Gtk::GestureClick const& gesture) { return gesture.get_button() == GDK_BUTTON_SECONDARY; });
        REQUIRE(secondaryClickPtr);

        auto const labels = collectAll<Gtk::Label>(columnView);
        auto const rowLabelIter = std::ranges::find_if(
          labels,
          [trackId1](Gtk::Label* label)
          {
            return label->get_text() == "track" && GPOINTER_TO_UINT(::g_object_get_data(
                                                     G_OBJECT(label->gobj()), kBoundTrackIdDataKey)) == trackId1.raw();
          });
        REQUIRE(rowLabelIter != labels.end());
        auto* const rowLabel = *rowLabelIter;
        REQUIRE(rowLabel->get_mapped());
        REQUIRE(rowLabel->get_width() > 0);
        REQUIRE(rowLabel->get_height() > 0);
        REQUIRE(controller.selectedTrackIds().empty());
        auto const optPoint =
          rowLabel->compute_point(columnView,
                                  Gdk::Graphene::Point{static_cast<float>(rowLabel->get_width()) / 2.0F,
                                                       static_cast<float>(rowLabel->get_height()) / 2.0F});
        REQUIRE(optPoint);
        auto const xPosition = static_cast<double>(optPoint->get_x());
        auto const yPosition = static_cast<double>(optPoint->get_y());

        // Direct signal emission proves the installed GTK binding and semantic
        // pick result, not native pointer delivery or gesture arbitration.
        ::g_signal_emit_by_name(secondaryClickPtr->gobj(), "released", 1, xPosition, yPosition);

        CHECK(requestCount == 1);
        CHECK(requestedX == xPosition);
        CHECK(requestedY == yPosition);
        CHECK(controller.selectedTrackIds() == std::vector<TrackId>{trackId1});
      }

      SECTION("secondary click on blank space does not request a track menu")
      {
        controller.configureActivation();
        std::size_t requestCount = 0;
        auto subscription = sigc::scoped_connection{
          controller.signalContextMenuRequested().connect([&](double, double) { ++requestCount; })};
        auto host = GtkWindowFixture{};
        host.window().set_default_size(400, 400);
        host.mount(columnView);
        host.present();
        auto const secondaryClickPtr = findControllerIf<Gtk::GestureClick>(
          columnView, [](Gtk::GestureClick const& gesture) { return gesture.get_button() == GDK_BUTTON_SECONDARY; });
        REQUIRE(secondaryClickPtr);
        REQUIRE(columnView.get_height() > 1);

        // Direct signal emission proves blank-pick binding only; it is not a
        // native pointer-delivery or gesture-arbitration witness.
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
