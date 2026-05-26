// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputSelector.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>

using namespace ao;
using namespace ao::gtk;
using namespace ao::gtk::test;

TEST_CASE("OutputSelector - smoke test", "[gtk][playback]")
{
  [[maybe_unused]] auto const app = ensureGtkApplication();
  auto fixture = GtkRuntimeFixture{};
  auto& playback = fixture.runtime().playback();

  auto const selector = OutputSelector{playback};

  SECTION("initializes without crashing")
  {
    drainGtkEvents();
  }
}
