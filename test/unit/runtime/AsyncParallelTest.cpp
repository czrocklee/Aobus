// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Exception.h>
#include <ao/async/Parallel.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::async;

  namespace
  {
    Task<> incrementTask(std::atomic<std::int32_t>* counter)
    {
      counter->fetch_add(1);
      co_return;
    }

    Task<> throwingTask()
    {
      throwException<Exception>("whenAll test failure");
      co_return;
    }

    // Completes only after both instances are in flight simultaneously (or a
    // 2-second deadline passes, leaving sawBoth false).
    Task<> rendezvousTask(std::atomic<std::int32_t>* inFlight, std::atomic<bool>* sawBoth)
    {
      inFlight->fetch_add(1);
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};

      while (inFlight->load() < 2 && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }

      if (inFlight->load() >= 2)
      {
        sawBoth->store(true);
      }

      co_return;
    }

    Task<> awaitAllTask(Runtime* runtime, std::vector<Task<>> tasks)
    {
      co_await whenAll(runtime, std::move(tasks));
    }
  } // namespace

  TEST_CASE("whenAll - completes after all tasks ran", "[runtime][unit][async]")
  {
    auto executor = MockExecutor{};
    auto runtime = Runtime{executor, 4};
    auto counter = std::atomic<std::int32_t>{0};

    auto tasks = std::vector<Task<>>{};

    for (std::int32_t index = 0; index < 8; ++index)
    {
      tasks.push_back(incrementTask(&counter));
    }

    runtime.spawn(awaitAllTask(&runtime, std::move(tasks))).get();

    CHECK(counter.load() == 8);
  }

  TEST_CASE("whenAll - empty task list completes immediately", "[runtime][unit][async]")
  {
    auto executor = MockExecutor{};
    auto runtime = Runtime{executor, 1};

    runtime.spawn(awaitAllTask(&runtime, {})).get();
  }

  TEST_CASE("whenAll - rethrows a task exception after all tasks finished", "[runtime][unit][async]")
  {
    auto executor = MockExecutor{};
    auto runtime = Runtime{executor, 2};
    auto counter = std::atomic<std::int32_t>{0};

    auto tasks = std::vector<Task<>>{};
    tasks.push_back(throwingTask());
    tasks.push_back(incrementTask(&counter));

    auto future = runtime.spawn(awaitAllTask(&runtime, std::move(tasks)));

    CHECK_THROWS_AS(future.get(), Exception);
    CHECK(counter.load() == 1);
  }

  TEST_CASE("whenAll - tasks run concurrently on the worker pool", "[runtime][unit][async]")
  {
    auto executor = MockExecutor{};
    auto runtime = Runtime{executor, 2};
    auto inFlight = std::atomic<std::int32_t>{0};
    auto sawBoth = std::atomic<bool>{false};

    auto tasks = std::vector<Task<>>{};
    tasks.push_back(rendezvousTask(&inFlight, &sawBoth));
    tasks.push_back(rendezvousTask(&inFlight, &sawBoth));

    runtime.spawn(awaitAllTask(&runtime, std::move(tasks))).get();

    CHECK(sawBoth.load());
  }

  TEST_CASE("whenAll - awaiting coroutine holds no pool thread", "[runtime][unit][async]")
  {
    // With a single-thread pool the coordinator must release its thread while
    // suspended in whenAll; a blocking wait would deadlock here instead of
    // letting the tasks run sequentially.
    auto executor = MockExecutor{};
    auto runtime = Runtime{executor, 1};
    auto counter = std::atomic<std::int32_t>{0};

    auto tasks = std::vector<Task<>>{};
    tasks.push_back(incrementTask(&counter));
    tasks.push_back(incrementTask(&counter));

    runtime.spawn(awaitAllTask(&runtime, std::move(tasks))).get();

    CHECK(counter.load() == 2);
  }
} // namespace ao::rt::test
