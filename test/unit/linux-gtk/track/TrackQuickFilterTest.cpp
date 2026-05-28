// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilter.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/entry.h>

namespace ao::gtk::test
{
  TEST_CASE("TrackQuickFilter - smoke test", "[gtk][track][viewmodel]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
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
} // namespace ao::gtk::test
