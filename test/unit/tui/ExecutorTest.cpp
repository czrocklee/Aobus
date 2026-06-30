// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Executor.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace ao::tui::test
{
  TEST_CASE("Executor - dispatch runs immediately on the owner thread", "[tui][unit][executor]")
  {
    auto screen = ftxui::ScreenInteractive::FixedSize(20, 5);
    auto executor = Executor{screen};
    bool ran = false;

    executor.dispatch(
      [&]
      {
        ran = true;
        CHECK(executor.isCurrent());
      });
    executor.dispatch({});

    CHECK(ran);
  }

  TEST_CASE("Executor - defer queues work onto the FTXUI loop", "[tui][unit][executor]")
  {
    auto screen = ftxui::ScreenInteractive::FixedSize(20, 5);
    auto executor = Executor{screen};
    bool ran = false;
    auto rendererPtr = ftxui::Renderer([] { return ftxui::text(""); });
    auto loop = ftxui::Loop{&screen, rendererPtr};

    executor.defer(
      [&]
      {
        ran = true;
        screen.ExitLoopClosure()();
      });
    loop.RunOnce();

    CHECK(ran);
  }
} // namespace ao::tui::test
