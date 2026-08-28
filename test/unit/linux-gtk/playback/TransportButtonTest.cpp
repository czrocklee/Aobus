// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TransportButton.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/uimodel/playback/command/PlaybackActions.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>

namespace ao::gtk::test
{
  TEST_CASE("TransportButton - renders transport action state and dispatches clicks", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();

    SECTION("PlayPause action maps initial view state to button attributes")
    {
      auto actions = uimodel::PlaybackActions{playback, [] {}};
      auto button =
        TransportButton{playback, actions, ao::test::englishMessageCatalog(), TransportButton::Action::PlayPause};
      auto* const gtkButton = dynamic_cast<Gtk::Button*>(&button.widget());
      REQUIRE(gtkButton != nullptr);
      auto windowFixture = GtkWindowFixture{};
      windowFixture.mount(button.widget());
      windowFixture.present();

      CHECK_FALSE(gtkButton->get_icon_name().empty());
      CHECK(gtkButton->has_css_class("ao-playback-button"));
      CHECK(hasAccessibleLabel(*gtkButton, "Play"));
    }

    SECTION("Play action routes clicks to selection playback callback")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      drainGtkEvents();
      bool playSelectionCalled = false;
      auto actions = uimodel::PlaybackActions{playback, [&playSelectionCalled] { playSelectionCalled = true; }};
      auto button =
        TransportButton{playback, actions, ao::test::englishMessageCatalog(), TransportButton::Action::Play, false};
      auto* const gtkButton = dynamic_cast<Gtk::Button*>(&button.widget());
      REQUIRE(gtkButton != nullptr);

      emitClicked(*gtkButton);
      CHECK(playSelectionCalled);
    }

    SECTION("The selected catalog supplies the accessible control name")
    {
      auto actions = uimodel::PlaybackActions{playback, [] {}};
      auto catalog = ao::test::messageCatalog("de-DE");
      auto button = TransportButton{playback, actions, catalog, TransportButton::Action::Previous};
      auto* const gtkButton = dynamic_cast<Gtk::Button*>(&button.widget());
      REQUIRE(gtkButton != nullptr);
      auto windowFixture = GtkWindowFixture{};
      windowFixture.mount(button.widget());
      windowFixture.present();
      CHECK(hasAccessibleLabel(*gtkButton, "Vorheriger Titel"));
    }
  }
} // namespace ao::gtk::test
