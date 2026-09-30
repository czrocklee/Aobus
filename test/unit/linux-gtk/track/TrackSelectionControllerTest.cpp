// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackSelectionController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/track/TrackSelectionControllerTestSupport.h"
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
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <glib-object.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/object.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    // One-consumer editable cell factory for the long-press section: each cell
    // is a Gtk::Stack whose "display" page is a label bound to the row's track
    // id and whose "edit" page is an entry, mirroring the inline-edit stack the
    // production long-press handler switches. Local to this test because only
    // the long-press regression exercises it.
    Glib::RefPtr<Gtk::SignalListItemFactory> createEditableCellFactory()
    {
      auto const factoryPtr = Gtk::SignalListItemFactory::create();

      factoryPtr->signal_setup().connect(
        [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
        {
          auto* const stack = Gtk::make_managed<Gtk::Stack>();
          stack->add(*Gtk::make_managed<Gtk::Label>(), "display");
          stack->add(*Gtk::make_managed<Gtk::Entry>(), "edit");
          itemPtr->set_child(*stack);
        });

      factoryPtr->signal_bind().connect(
        [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
        {
          auto* const stack = dynamic_cast<Gtk::Stack*>(itemPtr->get_child());
          auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr->get_item());

          if (stack == nullptr || rowPtr == nullptr)
          {
            return;
          }

          if (auto* const display = dynamic_cast<Gtk::Label*>(stack->get_child_by_name("display")); display != nullptr)
          {
            display->set_text("title");
            ::g_object_set_data(G_OBJECT(display->gobj()),
                                kBoundTrackIdDataKey,
                                GUINT_TO_POINTER(static_cast<guint>(rowPtr->trackId().raw())));
          }

          stack->set_visible_child("display");
        });

      factoryPtr->signal_unbind().connect(
        [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
        {
          if (auto* const stack = dynamic_cast<Gtk::Stack*>(itemPtr->get_child()); stack != nullptr)
          {
            if (auto* const display = dynamic_cast<Gtk::Label*>(stack->get_child_by_name("display"));
                display != nullptr)
            {
              ::g_object_set_data(G_OBJECT(display->gobj()), kBoundTrackIdDataKey, nullptr);
            }
          }
        });

      return factoryPtr;
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

      SECTION("restoreSelection adopts live ids in one notification without clearing existing rows")
      {
        selectionModelPtr->select_item(1, true);
        std::size_t selectionChangeCount = 0;
        auto subscription =
          sigc::scoped_connection{controller.signalSelectionChanged().connect([&] { ++selectionChangeCount; })};

        auto const restoredIds = std::vector{trackId4, TrackId{9999}, trackId1, trackId4};
        controller.restoreSelection(restoredIds);

        CHECK(controller.selectedTrackIds() == std::vector<TrackId>{trackId1, trackId2, trackId4});
        CHECK(selectionChangeCount == 1);

        controller.restoreSelection(restoredIds);
        CHECK(selectionChangeCount == 1);
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

        REQUIRE(controller.selectedTrackIds().empty());
        auto const point = cellCenterInView(columnView, "track", trackId1);
        auto const xPosition = static_cast<double>(point.get_x());
        auto const yPosition = static_cast<double>(point.get_y());

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

      SECTION("tags-cell double-click does not swallow the next activation")
      {
        appendTagsTestColumn(columnView);
        controller.configureActivation();
        selectionModelPtr->select_item(0, true);
        drainGtkEvents();

        std::size_t tagEditRequestCount = 0;
        auto tagEditIds = std::vector<TrackId>{};
        auto tagEditSubscription = sigc::scoped_connection{controller.signalTagEditRequested().connect(
          [&](std::vector<TrackId> const& ids, Gtk::Widget*)
          {
            ++tagEditRequestCount;
            tagEditIds = ids;
          })};
        auto activatedIds = std::vector<TrackId>{};
        auto activatedSubscription = sigc::scoped_connection{
          controller.signalTrackActivated().connect([&](TrackId trackId) { activatedIds.push_back(trackId); })};

        auto host = GtkWindowFixture{};
        host.window().set_default_size(400, 400);
        host.mount(columnView);
        host.present();
        auto const primaryClickPtr = findControllerIf<Gtk::GestureClick>(
          columnView, [](Gtk::GestureClick const& gesture) { return gesture.get_button() == GDK_BUTTON_PRIMARY; });
        REQUIRE(primaryClickPtr);

        auto const point = cellCenterInView(columnView, "tags", trackId1);

        // Direct gesture-signal emission proves only the pressed handler
        // binding. It carries no real GDK sequence, so the CLAIMED state is
        // inert here and this case cannot witness same-interaction gesture
        // arbitration; the native pointer test in
        // TrackSelectionControllerNativeInputTest.cpp proves that boundary.
        ::g_signal_emit_by_name(primaryClickPtr->gobj(),
                                "pressed",
                                2,
                                static_cast<double>(point.get_x()),
                                static_cast<double>(point.get_y()));
        CHECK(tagEditRequestCount == 1);
        CHECK(tagEditIds == std::vector<TrackId>{trackId1});
        CHECK(activatedIds.empty());

        // The claim already keeps that interaction's own activation from
        // firing, so no suppression may outlive it: the user's next activation
        // must play, whether it targets a row or the current selection.
        auto const optSecondIndex = modelPtr->indexOf(trackId2);
        REQUIRE(optSecondIndex);
        ::g_signal_emit_by_name(columnView.gobj(), "activate", static_cast<guint>(*optSecondIndex));
        CHECK(activatedIds == std::vector<TrackId>{trackId2});

        auto const keyControllerPtr = findController<Gtk::EventControllerKey>(columnView);
        REQUIRE(keyControllerPtr);
        gboolean handled = FALSE;
        ::g_signal_emit_by_name(keyControllerPtr->gobj(),
                                "key-pressed",
                                GDK_KEY_Return,
                                0U,
                                static_cast<GdkModifierType>(Gdk::ModifierType{}),
                                &handled);
        CHECK(activatedIds == std::vector<TrackId>{trackId2, trackId1});
      }

      SECTION("long-press on an editable cell switches to edit without suppressing later activation")
      {
        auto const editableColumnPtr = Gtk::ColumnViewColumn::create("Title", createEditableCellFactory());
        editableColumnPtr->set_fixed_width(160);
        columnView.append_column(editableColumnPtr);
        controller.configureActivation();
        selectionModelPtr->select_item(0, true);
        drainGtkEvents();

        auto activatedIds = std::vector<TrackId>{};
        auto activatedSubscription = sigc::scoped_connection{
          controller.signalTrackActivated().connect([&](TrackId trackId) { activatedIds.push_back(trackId); })};

        auto host = GtkWindowFixture{};
        host.window().set_default_size(400, 400);
        host.mount(columnView);
        host.present();
        auto const longPressPtr = findController<Gtk::GestureLongPress>(columnView);
        REQUIRE(longPressPtr);

        auto const point = cellCenterInView(columnView, "title", trackId1);
        auto* const display = columnView.pick(point.get_x(), point.get_y(), Gtk::PickFlags::NON_TARGETABLE);
        REQUIRE(display != nullptr);
        auto* const stack = dynamic_cast<Gtk::Stack*>(display->get_parent());
        REQUIRE(stack != nullptr);

        // Synthetic pressed emission proves the installed handler switches
        // the inline edit stack. It carries no real GDK hold sequence, so it
        // cannot witness native long-press arbitration.
        ::g_signal_emit_by_name(
          longPressPtr->gobj(), "pressed", static_cast<double>(point.get_x()), static_cast<double>(point.get_y()));
        drainGtkEvents();

        CHECK(stack->get_visible_child_name() == "edit");
        auto* const entry = dynamic_cast<Gtk::Entry*>(stack->get_child_by_name("edit"));
        REQUIRE(entry != nullptr);
        auto* const focus = host.window().get_focus();
        REQUIRE(focus != nullptr);
        CHECK((focus == entry || focus->is_ancestor(*entry)));

        stack->set_visible_child("display");
        columnView.grab_focus();
        drainGtkEvents();

        // After the editor closes, the column-view activation and key-handler
        // bindings stay live; no suppression state survives the long press.
        auto const optTrack2Index = modelPtr->indexOf(trackId2);
        REQUIRE(optTrack2Index);
        ::g_signal_emit_by_name(columnView.gobj(), "activate", static_cast<guint>(*optTrack2Index));
        CHECK(activatedIds == std::vector<TrackId>{trackId2});

        auto const keyControllerPtr = findController<Gtk::EventControllerKey>(columnView);
        REQUIRE(keyControllerPtr);
        gboolean handled = FALSE;
        ::g_signal_emit_by_name(keyControllerPtr->gobj(),
                                "key-pressed",
                                GDK_KEY_Return,
                                0U,
                                static_cast<GdkModifierType>(Gdk::ModifierType{}),
                                &handled);
        CHECK(activatedIds == std::vector<TrackId>{trackId2, trackId1});
      }

      columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
      drainGtkEvents();
    }

    drainGtkEvents();
  }
} // namespace ao::gtk::test
