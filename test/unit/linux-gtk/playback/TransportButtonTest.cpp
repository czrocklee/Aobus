// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/TransportButton.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/playback/TransportViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace ao::gtk::test
{
  TEST_CASE("TransportViewModel - wiring and lifetime", "[gtk][playback][viewmodel]")
  {
    auto fixture = GtkRuntimeFixture{};
    auto log = RenderLog<uimodel::playback::TransportViewState>{};

    SECTION("Subscriptions and refresh")
    {
      auto controller =
        std::make_unique<uimodel::playback::TransportViewModel>(fixture.runtime().playback(),
                                                                nullptr,
                                                                uimodel::playback::TransportAction::Shuffle,
                                                                nullptr,
                                                                false,
                                                                [&](auto const& view) { log.render(view); });

      // Initial render on construction
      CHECK(!log.empty());
      log.clear();

      // Event triggers render (can use PlaybackService directly)
      fixture.runtime().playback().setShuffleMode(rt::ShuffleMode::On);
      CHECK(log.states.size() == 1);

      log.clear();

      // Destroying controller disconnects events
      controller.reset();
      fixture.runtime().playback().setShuffleMode(rt::ShuffleMode::Off);
      CHECK(log.empty());
    }
  }

  TEST_CASE("TransportButton - GTK smoke test", "[gtk][playback][viewmodel]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    // Verify it instantiates and doesn't crash on applying state
    auto const button = TransportButton{fixture.runtime().playback(), nullptr, TransportButton::Action::PlayPause};
  }
} // namespace ao::gtk::test
