// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Executor.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

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
    [[maybe_unused]] auto loop = ftxui::Loop{&screen, rendererPtr};

    executor.defer(
      [&]
      {
        ran = true;
        screen.ExitLoopClosure()();
      });
    loop.RunOnce();

    CHECK(ran);
  }

  TEST_CASE("Executor - dispatch before the FTXUI loop starts can be drained", "[tui][unit][executor]")
  {
    auto screen = ftxui::ScreenInteractive::FixedSize(20, 5);
    auto executor = Executor{screen};
    auto ran = std::atomic_bool{false};

    auto worker = std::jthread{[&]
                               {
                                 executor.dispatch(
                                   [&]
                                   {
                                     CHECK(executor.isCurrent());
                                     ran = true;
                                     screen.ExitLoopClosure()();
                                   });
                               }};

    worker.join();
    CHECK_FALSE(ran.load());

    auto rendererPtr = ftxui::Renderer([] { return ftxui::text(""); });
    auto loop = ftxui::Loop{&screen, rendererPtr};
    executor.drainPendingTasks();

    CHECK(ran.load());
  }

  TEST_CASE("Executor - loop checkpoint recovers callbacks queued while terminal hooks are absent",
            "[tui][regression][executor][concurrency]")
  {
    auto screen = ftxui::ScreenInteractive::FixedSize(20, 5);
    auto executor = Executor{screen};
    auto rendererPtr = ftxui::Renderer([] { return ftxui::text(""); });
    auto loop = ftxui::Loop{&screen, rendererPtr};
    auto completed = std::vector<std::int32_t>{};
    bool handlingInput = false;
    auto enqueueFromWorker = [&](std::int32_t const value)
    {
      auto worker = std::jthread{[&, value]
                                 {
                                   executor.dispatch(
                                     [&, value]
                                     {
                                       CHECK(executor.isCurrent());
                                       CHECK_FALSE(handlingInput);
                                       completed.push_back(value);
                                     });
                                 }};
      worker.join();
    };

    screen.Post(
      [&]
      {
        handlingInput = true;
        screen.WithRestoredIO(
          [&]
          {
            screen.TrackMouse(false);
            enqueueFromWorker(1);
          })();
        // The queue is already nonempty, so a later callback cannot replace the lost wake.
        enqueueFromWorker(2);
        handlingInput = false;
      });
    loop.RunOnce();
    REQUIRE(completed.empty());

    // App drains here, after the FTXUI turn has unwound its input handlers.
    executor.drainPendingTasks();
    CHECK(completed == std::vector<std::int32_t>{1, 2});
    enqueueFromWorker(3);
    loop.RunOnce();
    CHECK(completed == std::vector<std::int32_t>{1, 2, 3});
  }
} // namespace ao::tui::test
