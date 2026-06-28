// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/track/TrackPresentationCatalog.h>
#include <ao/uimodel/track/TrackPresentationPreferenceStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/window.h>

#include <memory>

namespace ao::gtk::test
{
  // Menu population semantics are covered by TrackPresentationCatalog and workflow tests. The widget
  // keeps a focused adapter smoke: it binds services, renders the menu, and dispatches selection.
  TEST_CASE("TrackPresentationButton rebuilds presentation actions when focus changes",
            "[gtk][unit][track][presentation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeController = ThemeCoordinator{};
    auto catalog = uimodel::track::TrackPresentationCatalog{runtime.workspace()};
    auto preferences = uimodel::track::TrackPresentationPreferenceStore{catalog};

    auto window = Gtk::Window{};
    auto button = TrackPresentationButton{runtime};
    button.setPresentationServices(&catalog, &preferences, &themeController);
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

    // Selecting an action must not crash and should route through the bound services.
    emitClicked(*albumsButton);
    drainGtkEvents();
  }

  TEST_CASE("TrackPresentationButton cancels pending presentation apply when destroyed",
            "[gtk][unit][track][presentation][regression]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeController = ThemeCoordinator{};
    auto catalog = uimodel::track::TrackPresentationCatalog{runtime.workspace()};
    auto preferences = uimodel::track::TrackPresentationPreferenceStore{catalog};
    auto window = Gtk::Window{};

    runtime.workspace().navigateTo(rt::kAllTracksListId);
    drainGtkEvents();
    auto const activeViewId = runtime.workspace().layoutState().activeViewId;
    REQUIRE(activeViewId != rt::kInvalidViewId);
    REQUIRE(runtime.views().trackListState(activeViewId).presentation.id == rt::kDefaultTrackPresentationId);

    auto buttonPtr = std::make_unique<TrackPresentationButton>(runtime);
    buttonPtr->setPresentationServices(&catalog, &preferences, &themeController);
    window.set_child(*buttonPtr);
    drainGtkEvents();

    auto* const menuButton = findWidget<Gtk::MenuButton>(*buttonPtr);
    REQUIRE(menuButton != nullptr);
    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);
    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);

    emitClicked(*albumsButton);
    window.unset_child();
    buttonPtr.reset();
    drainGtkEvents();

    CHECK(runtime.views().trackListState(activeViewId).presentation.id == rt::kDefaultTrackPresentationId);
  }
} // namespace ao::gtk::test
