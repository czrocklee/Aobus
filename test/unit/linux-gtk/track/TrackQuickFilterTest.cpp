// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilter.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdkenums.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <giomm/listmodel.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/window.h>

#include <string_view>

namespace ao::gtk::test
{
  namespace
  {
    bool emitCompletionKey(Gtk::Entry& entry, guint const keyval)
    {
      auto const keyControllerPtr = findControllerIf<Gtk::EventControllerKey>(
        entry,
        [](Gtk::EventControllerKey const& controller)
        { return controller.get_propagation_phase() == Gtk::PropagationPhase::CAPTURE; });
      REQUIRE(keyControllerPtr);

      gboolean handled = FALSE;
      ::g_signal_emit_by_name(keyControllerPtr->gobj(),
                              "key-pressed",
                              keyval,
                              0U,
                              static_cast<GdkModifierType>(Gdk::ModifierType{}),
                              &handled);
      return handled == TRUE;
    }
  } // namespace

  TEST_CASE("TrackQuickFilter - renders action buttons and follows focused view", "[gtk][unit][track][quick-filter]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter =
      TrackQuickFilter{runtime.completion(), runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(filter);
    windowFixture.present();
    CHECK(filter.has_css_class("ao-quick-filter"));
    CHECK(filter.entry().has_css_class("ao-quick-filter-entry"));
    auto* const clearButton = findWidgetByClass<Gtk::Button>(filter, "ao-quick-filter-clear");
    auto* const createButton = findWidgetByClass<Gtk::Button>(filter, "ao-quick-filter-create");
    REQUIRE(clearButton != nullptr);
    REQUIRE(createButton != nullptr);
    CHECK(hasAccessibleLabel(*clearButton, "Clear filter"));
    CHECK(hasAccessibleLabel(*createButton, "Create List from current filter"));

    // Just verify it wires up and doesn't crash
    REQUIRE(runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks}));

    drainGtkEvents();
  }

  TEST_CASE("TrackQuickFilter - clear button clears current filter text", "[gtk][unit][track][quick-filter]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter =
      TrackQuickFilter{runtime.completion(), runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    auto* const clearButton = findWidgetByClass<Gtk::Button>(filter, "ao-quick-filter-clear");
    REQUIRE(clearButton != nullptr);

    filter.setText("artist == 'Muse'");
    drainGtkEvents();
    CHECK(filter.text() == "artist == 'Muse'");
    CHECK(clearButton->get_visible());

    emitClicked(*clearButton);
    drainGtkEvents();

    CHECK(filter.text().empty());
    CHECK_FALSE(clearButton->get_visible());
  }

  TEST_CASE("TrackQuickFilter - active class follows focus within the compound control",
            "[gtk][unit][track][quick-filter]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter =
      TrackQuickFilter{runtime.completion(), runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    CHECK_FALSE(filter.has_css_class("ao-quick-filter-active"));

    CHECK(emitFocusEnter(filter));
    CHECK(filter.has_css_class("ao-quick-filter-active"));

    CHECK(emitFocusLeave(filter));
    CHECK_FALSE(filter.has_css_class("ao-quick-filter-active"));
  }

  TEST_CASE("TrackQuickFilter - accepts query completion trigger text", "[gtk][unit][track][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter =
      TrackQuickFilter{runtime.completion(), runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    filter.setText("$al");
    filter.setPosition(3);
    drainGtkEvents();

    CHECK(filter.text() == "$al");
    CHECK(filter.position() == 3);
  }

  TEST_CASE("TrackQuickFilter - renders shared Quick-filter value completion", "[gtk][unit][track][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Completion Track", .artist = "Aimer"});

    auto window = Gtk::Window{};
    auto filter =
      TrackQuickFilter{runtime.completion(), runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    window.set_child(filter);
    auto* const popover = findWidget<Gtk::Popover>(filter.entry());
    REQUIRE(popover != nullptr);

    filter.setText("Aim");
    filter.setPosition(3);
    ::g_signal_emit_by_name(filter.entry().gobj(), "changed");
    drainGtkEvents();

    auto* const title = findWidgetByClass<Gtk::Label>(*popover, "ao-query-completion-row-title");
    REQUIRE(title != nullptr);
    CHECK(title->get_text() == "Aimer");

    CHECK(emitCompletionKey(filter.entry(), GDK_KEY_Return));
    CHECK(filter.text() == "\"Aimer\"");
    CHECK_FALSE(popover->get_visible());
  }
} // namespace ao::gtk::test
