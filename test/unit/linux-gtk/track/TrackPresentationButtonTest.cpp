// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
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
  // Catalog tests own menu contents; the widget owns binding and apply-before-persist.
  TEST_CASE("TrackPresentationButton - binds the active title and applies a menu selection",
            "[gtk][integration][track-presentation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{catalog, runtime.library().changes()};

    auto window = Gtk::Window{};
    auto button = TrackPresentationButton{runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    button.setPresentationServices(&catalog, &listPresentations, &themeCoordinator);
    window.set_child(button);
    auto* const menuButton = findWidget<Gtk::MenuButton>(button);
    REQUIRE(menuButton != nullptr);
    CHECK_FALSE(menuButton->get_sensitive());
    CHECK(menuButton->get_label() == "Presentation");

    REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();

    CHECK(menuButton->get_sensitive());
    CHECK(menuButton->get_label() == "Library");
    CHECK(button.get_valign() == Gtk::Align::CENTER);
    CHECK(menuButton->get_valign() == Gtk::Align::CENTER);
    CHECK(hasCssClass(*menuButton, "ao-presentation-trigger"));

    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);
    window.present();
    drainGtkEvents();
    menuButton->popup();
    drainGtkEvents();
    REQUIRE(popover->get_visible());

    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);
    CHECK(hasCssClass(*albumsButton, "ao-presentation-menu-item"));
    CHECK_FALSE(hasCssClass(*albumsButton, "ao-presentation-trigger"));

    // The adapter owns the apply-then-persist order: the runtime must accept the
    // change before the list preference records it.
    emitClicked(*albumsButton);
    drainGtkEvents();
    CHECK_FALSE(popover->get_visible());
    CHECK(menuButton->get_label() == "Albums");

    auto const activeViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(activeViewId != rt::kInvalidViewId);
    CHECK(runtime.views().trackListState(activeViewId).presentation.id == "albums");

    auto const optStored = listPresentations.presentationIdForList(rt::kAllTracksListId);
    REQUIRE(optStored);
    CHECK(*optStored == "albums");
    window.close();
    drainGtkEvents();
  }

  TEST_CASE("TrackPresentationButton - rebinding services drops the pending apply",
            "[gtk][integration][track-presentation][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{catalog, runtime.library().changes()};
    auto replacementPreferences = uimodel::ListPresentations{catalog, runtime.library().changes()};
    auto window = Gtk::Window{};

    auto const activeViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();

    auto button = TrackPresentationButton{runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    button.setPresentationServices(&catalog, &listPresentations, &themeCoordinator);
    window.set_child(button);
    drainGtkEvents();

    auto* const menuButton = findWidget<Gtk::MenuButton>(button);
    REQUIRE(menuButton != nullptr);
    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);
    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);

    // The queued apply belongs to the outgoing session. Rebinding must cancel
    // it, or it lands in the runtime with the incoming store recording it.
    emitClicked(*albumsButton);
    button.setPresentationServices(&catalog, &replacementPreferences, &themeCoordinator);
    drainGtkEvents();

    CHECK(runtime.views().trackListState(activeViewId).presentation.id == rt::kDefaultTrackPresentationId);
    CHECK_FALSE(listPresentations.presentationIdForList(rt::kAllTracksListId).has_value());
    CHECK_FALSE(replacementPreferences.presentationIdForList(rt::kAllTracksListId).has_value());
  }

  TEST_CASE("TrackPresentationButton - cancels pending presentation apply when destroyed",
            "[gtk][integration][track-presentation][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{catalog, runtime.library().changes()};
    auto window = Gtk::Window{};

    REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();
    auto const activeViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(activeViewId != rt::kInvalidViewId);
    REQUIRE(runtime.views().trackListState(activeViewId).presentation.id == rt::kDefaultTrackPresentationId);

    auto buttonPtr = std::make_unique<TrackPresentationButton>(
      runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog());
    buttonPtr->setPresentationServices(&catalog, &listPresentations, &themeCoordinator);
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

  TEST_CASE("TrackPresentationButton - refused deferred applies do not persist a preference",
            "[gtk][integration][track-presentation][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    themeCoordinator.setTheme(uimodel::ThemePreset::Modern);
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{catalog, runtime.library().changes()};
    auto window = Gtk::Window{};

    auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();
    auto button = TrackPresentationButton{runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    button.setPresentationServices(&catalog, &listPresentations, &themeCoordinator);
    window.set_child(button);
    window.present();
    drainGtkEvents();

    auto* const menuButton = findWidget<Gtk::MenuButton>(button);
    REQUIRE(menuButton != nullptr);
    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);
    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);

    auto const secondListId = ao::test::requireValue(
      runGtkTask(runtime, runtime.library().commands().createListAsync(rt::ListDraft{.name = "Other"})));

    emitClicked(*albumsButton);

    auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = secondListId}));
    auto const secondPresentationId = runtime.views().trackListState(secondViewId).presentation.id;
    drainGtkEvents();

    CHECK(runtime.views().trackListState(firstViewId).presentation.id == rt::kDefaultTrackPresentationId);
    CHECK(runtime.views().trackListState(secondViewId).presentation.id == secondPresentationId);
    CHECK_FALSE(listPresentations.presentationIdForList(secondListId).has_value());

    CHECK_FALSE(listPresentations.presentationIdForList(rt::kAllTracksListId).has_value());

    auto* const errorDialog = findAppDialogByTitle("Unable to Change Track View");
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
