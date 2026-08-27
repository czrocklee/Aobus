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
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto preferences = uimodel::ListPresentationPreferenceStore{catalog};

    auto window = Gtk::Window{};
    auto button = TrackPresentationButton{runtime, ao::test::englishMessageCatalog()};
    button.setPresentationServices(&catalog, &preferences, &themeCoordinator);
    window.set_child(button);

    REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
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

    // The adapter owns the apply-then-persist order: the runtime must accept the
    // change before the list preference records it.
    emitClicked(*albumsButton);
    drainGtkEvents();

    auto const activeViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(activeViewId != rt::kInvalidViewId);
    CHECK(runtime.views().trackListState(activeViewId).presentation.id == "albums");

    auto const optStored = preferences.presentationIdForList(rt::kAllTracksListId);
    REQUIRE(optStored);
    CHECK(*optStored == "albums");
  }

  TEST_CASE("TrackPresentationButton - rebinding services drops the pending apply",
            "[gtk][regression][track][presentation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto preferences = uimodel::ListPresentationPreferenceStore{catalog};
    auto replacementPreferences = uimodel::ListPresentationPreferenceStore{catalog};
    auto window = Gtk::Window{};

    auto const activeViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();

    auto button = TrackPresentationButton{runtime, ao::test::englishMessageCatalog()};
    button.setPresentationServices(&catalog, &preferences, &themeCoordinator);
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
    CHECK_FALSE(preferences.presentationIdForList(rt::kAllTracksListId).has_value());
    CHECK_FALSE(replacementPreferences.presentationIdForList(rt::kAllTracksListId).has_value());
  }

  TEST_CASE("TrackPresentationButton - cancels pending presentation apply when destroyed", "[gtk][unit][regression]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto preferences = uimodel::ListPresentationPreferenceStore{catalog};
    auto window = Gtk::Window{};

    REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();
    auto const activeViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(activeViewId != rt::kInvalidViewId);
    REQUIRE(runtime.views().trackListState(activeViewId).presentation.id == rt::kDefaultTrackPresentationId);

    auto buttonPtr = std::make_unique<TrackPresentationButton>(runtime, ao::test::englishMessageCatalog());
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

  TEST_CASE("TrackPresentationButton - refused deferred applies do not persist a preference",
            "[gtk][regression][track][presentation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    themeCoordinator.setTheme(uimodel::ThemePreset::Modern);
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto preferences = uimodel::ListPresentationPreferenceStore{catalog};
    auto window = Gtk::Window{};

    auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();
    auto button = TrackPresentationButton{runtime, ao::test::englishMessageCatalog()};
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

    auto const secondListId = ao::test::requireValue(
      runGtkTask(runtime, runtime.library().writer().createList(rt::ListDraft{.name = "Other"})));

    emitClicked(*albumsButton);

    SECTION("focus changes before the idle apply")
    {
      auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = secondListId}));
      auto const secondPresentationId = runtime.views().trackListState(secondViewId).presentation.id;
      drainGtkEvents();

      CHECK(runtime.views().trackListState(firstViewId).presentation.id == rt::kDefaultTrackPresentationId);
      CHECK(runtime.views().trackListState(secondViewId).presentation.id == secondPresentationId);
      CHECK_FALSE(preferences.presentationIdForList(secondListId).has_value());
    }

    CHECK_FALSE(preferences.presentationIdForList(rt::kAllTracksListId).has_value());

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
