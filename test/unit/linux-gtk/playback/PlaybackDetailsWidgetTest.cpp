// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace ao::gtk::test
{
  TEST_CASE("NowPlayingViewModel - wiring and lifetime", "[gtk][playback][viewmodel]")
  {
    auto fixture = GtkRuntimeFixture{};
    auto log = RenderLog<uimodel::playback::NowPlayingViewState>{};

    SECTION("Subscriptions and refresh")
    {
      auto controllerPtr = std::make_unique<uimodel::playback::NowPlayingViewModel>(
        fixture.runtime().playback(), [&](auto const& view) { log.render(view); });

      // Initial render on construction
      CHECK(!log.empty());
      log.clear();

      // Event triggers render
      fixture.runtime().playback().setShuffleMode(rt::ShuffleMode::On);

      fixture.runtime().playback().stop(); // Trigger onStopped
      CHECK(!log.states.empty());

      log.clear();

      // Destroying controller disconnects events
      controllerPtr.reset();
      fixture.runtime().playback().stop();
      CHECK(log.empty());
    }
  }
} // namespace ao::gtk::test
