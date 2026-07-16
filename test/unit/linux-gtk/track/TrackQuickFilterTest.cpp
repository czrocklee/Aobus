// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilter.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/window.h>

#include <string_view>

namespace ao::gtk::test
{
  TEST_CASE("TrackQuickFilter - renders action buttons and follows focused view", "[gtk][unit][track][quick-filter]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};
    CHECK(filter.has_css_class("ao-quick-filter"));
    CHECK(filter.entry().has_css_class("ao-quick-filter-entry"));

    // Just verify it wires up and doesn't crash
    REQUIRE(runtime.workspace().navigateTo(rt::GlobalViewKind::AllTracks));

    drainGtkEvents();
  }

  TEST_CASE("TrackQuickFilter - clear button clears current filter text", "[gtk][unit][track][quick-filter]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};
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

    auto filter = TrackQuickFilter{runtime};
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

    auto filter = TrackQuickFilter{runtime};
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
    addRuntimeTrack(runtime, {.title = "Completion Track", .artist = "Aimer"});

    auto window = Gtk::Window{};
    auto filter = TrackQuickFilter{runtime};
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
  }
} // namespace ao::gtk::test
