// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkNativeInputTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/track/TrackSelectionControllerTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackSelectionController.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gtkmm/columnview.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

// Xlib macros must be included after all GTK and glibmm headers.
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

namespace ao::gtk::test
{
  namespace
  {
    // One-consumer modifier ownership for the Ctrl-deselect native regression.
    class [[nodiscard]] ScopedControlKey final
    {
    public:
      ScopedControlKey()
      {
        requireOwnedGtkDisplay();
        _displayPtr.reset(::XOpenDisplay(nullptr));
        REQUIRE(_displayPtr);
        _key = ::XKeysymToKeycode(_displayPtr.get(), XK_Control_L);
        REQUIRE(_key != 0);
      }

      ~ScopedControlKey()
      {
        if (_held)
        {
          ::XTestFakeKeyEvent(_displayPtr.get(), _key, False, CurrentTime);
          ::XSync(_displayPtr.get(), False);
        }
      }

      ScopedControlKey(ScopedControlKey const&) = delete;
      ScopedControlKey& operator=(ScopedControlKey const&) = delete;
      ScopedControlKey(ScopedControlKey&&) = delete;
      ScopedControlKey& operator=(ScopedControlKey&&) = delete;

      void hold()
      {
        _held = true;
        REQUIRE(::XTestFakeKeyEvent(_displayPtr.get(), _key, True, CurrentTime) != 0);
        ::XSync(_displayPtr.get(), False);
      }

      void release()
      {
        REQUIRE(::XTestFakeKeyEvent(_displayPtr.get(), _key, False, CurrentTime) != 0);
        ::XSync(_displayPtr.get(), False);
        _held = false;
      }

    private:
      std::unique_ptr<::Display, decltype(&::XCloseDisplay)> _displayPtr{nullptr, &::XCloseDisplay};
      ::KeyCode _key = 0;
      bool _held = false;
    };

    void clickAt(GtkNativePointerFixture& native, double const x, double const y)
    {
      native.movePointerTo(x, y);
      native.press();
      native.release();
    }

    void doubleClickAt(GtkNativePointerFixture& native, double const x, double const y)
    {
      clickAt(native, x, y);
      native.press();
      native.release();
    }
  } // namespace

  // Real pointer input through the portal-owned Xvfb/XTest display. The
  // synthetic case in TrackSelectionControllerTest.cpp proves the handler
  // binding only; only a genuine GDK double-click sequence can witness the
  // CLAIMED gesture arbitration against the column view's row activation.
  TEST_CASE("TrackSelectionController - native tags-cell double click claims only its own activation",
            "[gtk][unit][track][selection]")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId1 = kInvalidTrackId;
    auto trackId2 = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      {
        trackId1 = library::test::addTrackWithUniqueFixtureUri(
          musicLibrary, library::test::TrackSpec{.title = "Track 1", .duration = std::chrono::minutes{2}});
        trackId2 = library::test::addTrackWithUniqueFixtureUri(
          musicLibrary, library::test::TrackSpec{.title = "Track 2", .duration = std::chrono::minutes{3}});
      }};
    auto cache = TrackRowCache{fixture.runtime().library(), ao::test::englishMessageCatalog()};
    auto modelPtr = TrackListModel::create(cache);
    auto selectionModelPtr = Gtk::MultiSelection::create(modelPtr);

    auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
    sourcePtr->addInitial(trackId1);
    sourcePtr->addInitial(trackId2);
    auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
      fixture.runtime().views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
    modelPtr->bindProjection(projectionPtr);
    drainGtkEvents();

    auto columnView = Gtk::ColumnView{};
    appendTestColumn(columnView);
    appendTagsTestColumn(columnView);
    columnView.set_model(selectionModelPtr);

    auto controller = TrackSelectionController{columnView, modelPtr, selectionModelPtr};
    controller.configureActivation();

    std::size_t tagEditRequestCount = 0;
    auto tagEditIds = std::vector<TrackId>{};
    auto* tagEditTarget = static_cast<Gtk::Widget*>(nullptr);
    auto const tagEditSubscription = sigc::scoped_connection{controller.signalTagEditRequested().connect(
      [&](std::vector<TrackId> const& ids, Gtk::Widget* targetWidget)
      {
        ++tagEditRequestCount;
        tagEditIds = ids;
        tagEditTarget = targetWidget;
      })};
    auto activatedIds = std::vector<TrackId>{};
    auto const activatedSubscription = sigc::scoped_connection{
      controller.signalTrackActivated().connect([&](TrackId trackId) { activatedIds.push_back(trackId); })};

    auto host = GtkWindowFixture{};
    host.window().set_default_size(400, 400);
    host.mount(columnView);
    host.present();
    auto native = GtkNativePointerFixture{columnView};

    SECTION("requests one tags edit for the clicked row without activating it")
    {
      auto const point = cellCenterInView(columnView, "tags", trackId1);

      // No row is selected beforehand: a genuine tags double click must first
      // click-select the row and then claim the second press, so the edit
      // request carries the live selection this very interaction produced.
      doubleClickAt(native, point.get_x(), point.get_y());
      drainGtkEvents();

      CHECK(tagEditRequestCount == 1);
      CHECK(tagEditIds == std::vector<TrackId>{trackId1});
      REQUIRE(tagEditTarget != nullptr);
      CHECK(hasCssClass(*tagEditTarget, kTagsCellCssClass));

      // The claim keeps this interaction's own row activation from firing.
      CHECK(activatedIds.empty());
      CHECK(native.pressCount() == 2);
      CHECK(native.releaseCount() == 2);
    }

    SECTION("leaves the next genuine plain double click activating exactly once")
    {
      auto const tagsPoint = cellCenterInView(columnView, "tags", trackId1);
      doubleClickAt(native, tagsPoint.get_x(), tagsPoint.get_y());
      drainGtkEvents();
      REQUIRE(tagEditRequestCount == 1);
      REQUIRE(activatedIds.empty());

      auto const plainPoint = cellCenterInView(columnView, "track", trackId2);
      doubleClickAt(native, plainPoint.get_x(), plainPoint.get_y());

      REQUIRE(tryPumpGtkEventsUntil([&] { return activatedIds.size() == 1; }, std::chrono::milliseconds{2000}));

      // Exactly one activation for the freshly double-clicked row: the claim
      // of the previous interaction neither swallowed this gesture nor
      // suppressed the activation it produces.
      CHECK(activatedIds == std::vector<TrackId>{trackId2});
      CHECK(tagEditRequestCount == 1);
      CHECK(native.pressCount() == 4);
      CHECK(native.releaseCount() == 4);
    }

    SECTION("a rapid third click on the tags cell neither edits again nor activates the row")
    {
      auto const point = cellCenterInView(columnView, "tags", trackId1);
      doubleClickAt(native, point.get_x(), point.get_y());
      native.press();
      auto const activationsAfterThirdPress = activatedIds.size();
      native.release();
      drainGtkEvents();
      CAPTURE(activationsAfterThirdPress);

      // A rapid tags multi-click requests one editor and never activates the
      // row, including on the third press after the initial edit request.
      CHECK(tagEditRequestCount == 1);
      CHECK(activatedIds.empty());
      CHECK(native.pressCount() == 3);
      CHECK(native.releaseCount() == 3);
    }

    SECTION("Ctrl tags double click neither edits nor activates after deselecting the row")
    {
      REQUIRE(selectionModelPtr->select_item(0U, true));
      REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{trackId1});
      auto const point = cellCenterInView(columnView, "tags", trackId1);
      auto control = ScopedControlKey{};
      control.hold();
      clickAt(native, point.get_x(), point.get_y());
      auto const selectionAfterFirstClick = controller.selectedTrackIds().size();
      CAPTURE(selectionAfterFirstClick);
      REQUIRE(controller.selectedTrackIds().empty());
      native.press();
      native.release();
      drainGtkEvents();
      CAPTURE(tagEditRequestCount, activatedIds.size());
      CHECK(tagEditRequestCount == 0U);
      CHECK(activatedIds.empty());
      CHECK(controller.selectedTrackIds().empty());
      CHECK(native.pressCount() == 2);
      CHECK(native.releaseCount() == 2);

      control.release();
      auto const plainPoint = cellCenterInView(columnView, "track", trackId2);
      doubleClickAt(native, plainPoint.get_x(), plainPoint.get_y());
      REQUIRE(tryPumpGtkEventsUntil([&] { return activatedIds.size() == 1; }, std::chrono::milliseconds{2000}));
      CHECK(activatedIds == std::vector<TrackId>{trackId2});
    }

    columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
    drainGtkEvents();
  }
} // namespace ao::gtk::test
