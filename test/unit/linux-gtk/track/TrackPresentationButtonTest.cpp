// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackPresentationStore.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("TrackPresentationButton - menu population", "[gtk][track][presentation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto presentationStore = TrackPresentationStore{runtime.workspace()};

    auto window = Gtk::Window{};
    auto button = TrackPresentationButton{runtime};
    button.setPresentationStore(&presentationStore);
    window.set_child(button);

    SECTION("initial state")
    {
      // Verify it doesn't crash
    }

    SECTION("populates on focus change")
    {
      runtime.workspace().navigateTo(rt::kAllTracksListId);
      drainGtkEvents();
    }
  }
} // namespace ao::gtk::test
