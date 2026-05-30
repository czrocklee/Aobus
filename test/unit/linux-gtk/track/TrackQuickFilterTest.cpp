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
#include <gtkmm/entry.h>

namespace ao::gtk::test
{
  TEST_CASE("TrackQuickFilter - smoke test", "[gtk][track][viewmodel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};
    auto* const gtkEntry = dynamic_cast<Gtk::Entry*>(&filter);
    REQUIRE(gtkEntry);

    // Just verify it wires up and doesn't crash
    auto const reply = runtime.views().createView({.listId = ListId{1}});
    runtime.workspace().setFocusedView(reply.viewId);

    drainGtkEvents();
  }

  TEST_CASE("TrackQuickFilter - typing does not overwrite presentation", "[gtk][track][viewmodel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};
    auto* const gtkEntry = dynamic_cast<Gtk::Entry*>(&filter);

    auto config = rt::TrackListViewConfig{.listId = ListId{1}};
    config.optPresentation = rt::defaultTrackPresentationSpec();
    config.optPresentation->id = "custom";
    auto const reply = runtime.views().createView(config);
    runtime.workspace().setFocusedView(reply.viewId);

    drainGtkEvents();

    gtkEntry->set_text("artist == 'Muse'");
    // Simulate activate (Enter key) if it requires it, or maybe it updates on changed?
    gtkEntry->activate();
    drainGtkEvents();

    // Wait for debounce timer (200ms)
    ::g_usleep(static_cast<gulong>(250) * 1000);
    drainGtkEvents();

    auto const state = runtime.views().trackListState(reply.viewId);
    CHECK(state.filterExpression == "artist == 'Muse'");
    CHECK(state.presentation.id == "custom"); // Should remain unchanged
  }
} // namespace ao::gtk::test
