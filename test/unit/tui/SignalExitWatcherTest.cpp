// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/SignalExitWatcher.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace ao::tui::test
{
  TEST_CASE("SignalExitWatcher - exit requests reach the registered callback", "[tui][unit][signal-exit]")
  {
    auto mutex = std::mutex{};
    auto cv = std::condition_variable{};
    std::size_t exitCount = 0;
    auto watcher = SignalExitWatcher{[&]
                                     {
                                       {
                                         auto const lock = std::scoped_lock{mutex};
                                         ++exitCount;
                                       }

                                       cv.notify_all();
                                     }};

    watcher.requestExit();
    watcher.requestExit();

    auto lock = std::unique_lock{mutex};
    REQUIRE(cv.wait_for(lock, std::chrono::seconds{2}, [&] { return exitCount == 2; }));
  }

  TEST_CASE("SignalExitWatcher - callback can destroy its watcher", "[tui][regression][signal-exit][lifecycle]")
  {
    auto mutex = std::mutex{};
    auto cv = std::condition_variable{};
    bool resetCompleted = false;
    auto watcherPtr = std::unique_ptr<SignalExitWatcher>{};
    watcherPtr = std::make_unique<SignalExitWatcher>(
      [&]
      {
        watcherPtr.reset();

        {
          auto const lock = std::scoped_lock{mutex};
          resetCompleted = true;
        }

        cv.notify_all();
      });

    auto* const watcher = watcherPtr.get();
    watcher->requestExit();

    auto lock = std::unique_lock{mutex};
    REQUIRE(cv.wait_for(lock, std::chrono::seconds{2}, [&] { return resetCompleted; }));
    CHECK(watcherPtr == nullptr);
  }
} // namespace ao::tui::test
