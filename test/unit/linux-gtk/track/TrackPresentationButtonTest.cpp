// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  // Menu population semantics (which presets appear, ordering, active marker) are covered by
  // TrackPresentationViewModel's tests. The widget keeps a single smoke: it binds its store and
  // rebuilds its menu on a focus change.
  TEST_CASE("TrackPresentationButton rebuilds presentation actions when focus changes",
            "[gtk][unit][track][presentation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeController = ThemeCoordinator{};
    auto presentationStore = uimodel::track::TrackPresentationViewModel{runtime.workspace()};

    auto window = Gtk::Window{};
    auto button = TrackPresentationButton{runtime};
    button.setPresentationStore(&presentationStore, &themeController);
    window.set_child(button);

    runtime.workspace().navigateTo(rt::kAllTracksListId);
    drainGtkEvents();

    auto* const menuButton = findWidget<Gtk::MenuButton>(button);
    REQUIRE(menuButton != nullptr);
    CHECK(menuButton->get_sensitive());
    CHECK(button.get_valign() == Gtk::Align::CENTER);
    CHECK(menuButton->get_valign() == Gtk::Align::CENTER);
    CHECK(hasCssClass(*menuButton, "ao-presentation-trigger"));

    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);

    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);
    CHECK(hasCssClass(*albumsButton, "ao-presentation-menu-item"));
    CHECK_FALSE(hasCssClass(*albumsButton, "ao-presentation-trigger"));

    emitClicked(*albumsButton);
    drainGtkEvents();

    auto const optPresentationId = presentationStore.presentationIdForList(rt::kAllTracksListId);
    REQUIRE(optPresentationId.has_value());
    CHECK(*optPresentationId == "albums");
    auto const activeViewId = runtime.workspace().layoutState().activeViewId;
    REQUIRE(activeViewId != rt::kInvalidViewId);
    CHECK(runtime.views().trackListState(activeViewId).presentation.id == "albums");
  }
} // namespace ao::gtk::test
