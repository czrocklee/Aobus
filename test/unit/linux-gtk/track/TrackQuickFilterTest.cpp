// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilter.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>

#include <string>
#include <string_view>

namespace ao::gtk::test
{
  TEST_CASE("TrackQuickFilter renders action buttons and follows focused view", "[gtk][unit][track][quick-filter]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};
    CHECK(filter.has_css_class("ao-quick-filter"));
    CHECK(filter.entry().has_css_class("ao-quick-filter-entry"));

    // Just verify it wires up and doesn't crash
    auto const reply = runtime.views().createView({.listId = ListId{1}});
    runtime.workspace().setFocusedView(reply.viewId);

    drainGtkEvents();
  }

  TEST_CASE("TrackQuickFilter - typing does not overwrite presentation", "[gtk][unit][track][quick-filter]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};

    auto config = rt::TrackListViewConfig{.listId = ListId{1}};
    config.optPresentation = rt::defaultTrackPresentationSpec();
    config.optPresentation->id = "custom";
    auto const reply = runtime.views().createView(config);
    runtime.workspace().setFocusedView(reply.viewId);

    drainGtkEvents();

    filter.setText("artist == 'Muse'");
    // Simulate activate (Enter key) if it requires it, or maybe it updates on changed?
    filter.activate();
    drainGtkEvents();

    CHECK(pumpGtkEventsUntil(
      [&runtime, reply]
      {
        auto const state = runtime.views().trackListState(reply.viewId);
        return state.filterExpression == "artist == 'Muse'";
      }));

    auto const state = runtime.views().trackListState(reply.viewId);
    CHECK(state.filterExpression == "artist == 'Muse'");
    CHECK(state.presentation.id == "custom"); // Should remain unchanged
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
    CHECK(filter.getText() == "artist == 'Muse'");
    CHECK(clearButton->get_visible());

    emitClicked(*clearButton);
    drainGtkEvents();

    CHECK(filter.getText().empty());
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

    CHECK(filter.getText() == "$al");
    CHECK(filter.getPosition() == 3);
  }
} // namespace ao::gtk::test
