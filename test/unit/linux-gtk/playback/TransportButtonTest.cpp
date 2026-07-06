// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TransportButton.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>

namespace ao::gtk::test
{
  TEST_CASE("TransportButton renders transport action state and dispatches clicks", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    SECTION("PlayPause action maps initial view state to button attributes")
    {
      auto commands = uimodel::PlaybackCommandSurface{fixture.runtime().playback(), nullptr, [] {}};
      auto button = TransportButton{fixture.runtime().playback(), commands, TransportButton::Action::PlayPause};
      auto* const gtkButton = dynamic_cast<Gtk::Button*>(&button.widget());
      REQUIRE(gtkButton != nullptr);

      CHECK_FALSE(gtkButton->get_icon_name().empty());
      CHECK(gtkButton->has_css_class("ao-playback-button"));
    }

    SECTION("Play action routes clicks to selection playback callback")
    {
      rt::test::addReadyAudioProvider(fixture.runtime().playback());
      bool playSelectionCalled = false;
      auto commands = uimodel::PlaybackCommandSurface{
        fixture.runtime().playback(), nullptr, [&playSelectionCalled] { playSelectionCalled = true; }};
      auto button = TransportButton{fixture.runtime().playback(), commands, TransportButton::Action::Play, false};
      auto* const gtkButton = dynamic_cast<Gtk::Button*>(&button.widget());
      REQUIRE(gtkButton != nullptr);

      emitClicked(*gtkButton);
      CHECK(playSelectionCalled);
    }
  }
} // namespace ao::gtk::test
