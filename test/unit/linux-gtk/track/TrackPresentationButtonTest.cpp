// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/window.h>

#include <memory>

namespace ao::gtk::test
{
  // Menu population semantics are covered by TrackPresentationCatalog and workflow tests. The widget
  // keeps a focused adapter smoke: it binds services, renders the menu, and dispatches selection.
  TEST_CASE("TrackPresentationButton - rebuilds presentation actions when focus changes",
            "[gtk][unit][track][presentation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace()};
    auto preferences = uimodel::ListPresentationPreferenceStore{catalog};

    auto window = Gtk::Window{};
    auto button = TrackPresentationButton{runtime};
    button.setPresentationServices(&catalog, &preferences, &themeCoordinator);
    window.set_child(button);

    REQUIRE(runtime.workspace().navigateTo(rt::kAllTracksListId));
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

  TEST_CASE("TrackPresentationButton - cancels pending presentation apply when destroyed", "[gtk][unit][regression]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace()};
    auto preferences = uimodel::ListPresentationPreferenceStore{catalog};
    auto window = Gtk::Window{};

    REQUIRE(runtime.workspace().navigateTo(rt::kAllTracksListId));
    drainGtkEvents();
    auto const activeViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(activeViewId != rt::kInvalidViewId);
    REQUIRE(runtime.views().trackListState(activeViewId).presentation.id == rt::kDefaultTrackPresentationId);

    auto buttonPtr = std::make_unique<TrackPresentationButton>(runtime);
    buttonPtr->setPresentationServices(&catalog, &preferences, &themeCoordinator);
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

  TEST_CASE("TrackPresentationButton - focus change before idle does not mutate the new active view",
            "[gtk][regression][track][presentation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    themeCoordinator.setTheme(uimodel::ThemePreset::Modern);
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace()};
    auto preferences = uimodel::ListPresentationPreferenceStore{catalog};
    auto window = Gtk::Window{};

    auto const firstViewId = ao::test::requireValue(runtime.workspace().navigateTo(rt::kAllTracksListId));
    drainGtkEvents();
    auto button = TrackPresentationButton{runtime};
    button.setPresentationServices(&catalog, &preferences, &themeCoordinator);
    window.set_child(button);
    window.present();
    drainGtkEvents();

    auto* const menuButton = findWidget<Gtk::MenuButton>(button);
    REQUIRE(menuButton != nullptr);
    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);
    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);

    emitClicked(*albumsButton);
    auto const secondListId = ao::test::requireValue(runtime.library().writer().createList(
      rt::LibraryWriter::ListDraft{.kind = rt::LibraryWriter::ListKind::Manual, .name = "Other"}));
    auto const secondViewId = ao::test::requireValue(runtime.workspace().navigateTo(secondListId));
    auto const secondPresentationId = runtime.views().trackListState(secondViewId).presentation.id;
    drainGtkEvents();

    CHECK(runtime.views().trackListState(firstViewId).presentation.id == rt::kDefaultTrackPresentationId);
    CHECK(runtime.views().trackListState(secondViewId).presentation.id == secondPresentationId);

    AppDialog* errorDialog = nullptr;

    for (auto* const topLevel : Gtk::Window::list_toplevels())
    {
      if (auto* const dialog = dynamic_cast<AppDialog*>(topLevel);
          dialog != nullptr && dialog->get_title() == "Unable to Change Track View")
      {
        errorDialog = dialog;
        break;
      }
    }

    REQUIRE(errorDialog != nullptr);
    CHECK(errorDialog->get_transient_for() == &window);
    CHECK(errorDialog->has_css_class("ao-theme-modern"));
    themeCoordinator.setTheme(uimodel::ThemePreset::Classic);
    CHECK_FALSE(errorDialog->has_css_class("ao-theme-modern"));
    CHECK(errorDialog->has_css_class("ao-theme-classic"));
    errorDialog->response(Gtk::ResponseType::CLOSE);
    window.close();
    drainGtkEvents();
  }
} // namespace ao::gtk::test
