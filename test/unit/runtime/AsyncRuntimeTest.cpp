// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include <ao/async/ImmediateExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/CorePrimitives.h>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <thread>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::async;

  namespace
  {
    Task<std::thread::id> pingPongTask(Runtime* runtime, AsyncTestState<int> counter)
    {
      co_await runtime->resumeOnWorker();
      // Now on worker thread — the thread switch is the behavior under test.
      (*counter)++;

      co_await runtime->resumeOnCallbackExecutor();
      // Now back on UI (ImmediateExecutor for tests)
      (*counter)++;

      co_return std::this_thread::get_id();
    }

    Task<void> failingTask(Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      throw std::runtime_error{"Test failure"};
    }
  }

  TEST_CASE("Async runtime - Basic spawn and wait", "[async][unit][runtime]")
  {
    auto executor = ImmediateExecutor{};
    auto runtime = Runtime{executor};
    auto counter = AsyncTestState<int>::create(0);

    auto future = runtime.spawn(pingPongTask(&runtime, counter));
    auto const result = future.get();

    REQUIRE(result != std::this_thread::get_id());
    REQUIRE(counter.get() == 2);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("Async runtime - Exception handling", "[async][unit][runtime]")
  {
    auto executor = ImmediateExecutor{};
    auto runtime = Runtime{executor};

    // Logging version - should not crash
    REQUIRE_NOTHROW(runtime.spawnLogged(failingTask(&runtime)));

    // Future version - should throw when getting result
    auto future = runtime.spawn(failingTask(&runtime));
    REQUIRE_THROWS_AS(future.get(), std::runtime_error);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("Immediate executor - defer is FIFO and never reenters the current task", "[async][unit][runtime]")
  {
    auto executor = ImmediateExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer(
          [&]
          {
            order.push_back(3);
            executor.defer([&] { order.push_back(5); });
          });
        executor.defer([&] { order.push_back(4); });
        order.push_back(2);
      });

    REQUIRE(order == std::vector<int>{1, 2, 3, 4, 5});

    executor.dispatch([&] { order.push_back(6); });
    REQUIRE(order.back() == 6);
  }

  TEST_CASE("Immediate executor - a throwing task does not wedge the queue", "[async][unit][runtime]")
  {
    auto executor = ImmediateExecutor{};
    auto order = std::vector<int>{};

    REQUIRE_THROWS_AS(executor.defer(
                        [&]
                        {
                          executor.defer([&] { order.push_back(1); });
                          throw std::runtime_error{"boom"};
                        }),
                      std::runtime_error);

    // The task deferred before the throw stayed queued and runs in the next turn.
    REQUIRE(order.empty());
    executor.defer([&] { order.push_back(2); });
    REQUIRE(order == std::vector<int>{1, 2});
  }

  TEST_CASE("Signal - re-posting during a posted emission runs after the current emission", "[async][unit][runtime]")
  {
    auto executor = ImmediateExecutor{};
    auto signal = Signal<int>{};
    auto order = std::vector<int>{};

    auto subscription = signal.connect(
      [&](int value)
      {
        order.push_back(value);

        if (value == 1)
        {
          signal.post(executor, 2);
          // The re-posted emission must not run inline inside this handler.
          order.push_back(-1);
        }
      });

    signal.post(executor, 1);

    REQUIRE(order == std::vector<int>{1, -1, 2});
  }

  TEST_CASE("Signal - handlers connected during emission join the next emission", "[async][unit][runtime]")
  {
    auto signal = Signal<int>{};
    auto order = std::vector<int>{};
    auto subscriptions = std::vector<Subscription>{};

    subscriptions.push_back(signal.connect(
      [&](int value)
      {
        order.push_back(value);

        if (value == 1)
        {
          // Enough connects to force handler-vector reallocation while the emit loop is live.
          for (auto added = 0; added < 16; ++added)
          {
            subscriptions.push_back(signal.connect([&](int inner) { order.push_back(100 + inner); }));
          }
        }
      }));

    signal.emit(1);
    REQUIRE(order == std::vector<int>{1});

    signal.emit(2);
    REQUIRE(order.size() == 1 + 1 + 16);
    CHECK(order[1] == 2);
    CHECK(order.back() == 102);
  }
} // namespace ao::rt::test
