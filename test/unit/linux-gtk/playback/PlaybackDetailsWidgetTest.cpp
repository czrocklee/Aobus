// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <ao/rt/StateTypes.h>
#include <catch2/catch_test_macros.hpp>
#include <memory>

using namespace ao;
using namespace ao::gtk;
using namespace ao::gtk::test;

TEST_CASE("NowPlayingViewModel - wiring and lifetime", "[gtk][playback][viewmodel]")
{
  auto fixture = GtkRuntimeFixture{};
  auto log = RenderLog<ao::uimodel::playback::NowPlayingViewState>{};

  SECTION("Subscriptions and refresh")
  {
    auto controller = std::make_unique<ao::uimodel::playback::NowPlayingViewModel>(
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
    controller.reset();
    fixture.runtime().playback().stop();
    CHECK(log.empty());
  }
}
