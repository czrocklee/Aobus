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

#include <cstdint>

namespace ao::gtk::test
{
  TEST_CASE("TransportButton - projects initial and localized action presentation", "[gtk][unit][playback]")
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

      CHECK(gtkButton->get_icon_name() == "media-playback-start-symbolic");
      CHECK(gtkButton->has_css_class("ao-playback-button"));
      CHECK(gtkButton->get_tooltip_text() == "Play");
      CHECK(hasAccessibleLabel(*gtkButton, "Play"));
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
      CHECK(gtkButton->get_icon_name() == "media-skip-backward-symbolic");
      CHECK(gtkButton->get_tooltip_text() == "Vorheriger Titel");
      CHECK(hasAccessibleLabel(*gtkButton, "Vorheriger Titel"));
    }
  }

  TEST_CASE("TransportButton - routes a play click once to selection playback", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    rt::test::addReadyAudioProvider(fixture.runtime());
    drainGtkEvents();
    std::int32_t playSelectionCalls = 0;
    auto actions = uimodel::PlaybackActions{playback, [&] { ++playSelectionCalls; }};
    auto button =
      TransportButton{playback, actions, ao::test::englishMessageCatalog(), TransportButton::Action::Play, false};
    auto* const gtkButton = dynamic_cast<Gtk::Button*>(&button.widget());
    REQUIRE(gtkButton != nullptr);
    REQUIRE(playSelectionCalls == 0);

    emitClicked(*gtkButton);
    CHECK(playSelectionCalls == 1);
  }
} // namespace ao::gtk::test
