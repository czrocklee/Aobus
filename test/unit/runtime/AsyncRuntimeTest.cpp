// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include "runtime/ImmediateControlExecutor.h"
#include "runtime/async/Runtime.h"
#include "runtime/async/Task.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace ao::rt::test
{
  using namespace ao::rt::async;

  namespace
  {
    Task<std::thread::id> pingPongTask(Runtime* runtime, AsyncTestState<int> counter)
    {
      co_await runtime->resumeOnWorker();
      // Now on worker thread
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      (*counter)++;

      co_await runtime->resumeOnControl();
      // Now back on UI (ImmediateControlExecutor for tests)
      (*counter)++;

      co_return std::this_thread::get_id();
    }

    Task<void> failingTask(Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      throw std::runtime_error("Test failure");
    }
  }

  TEST_CASE("Async runtime - Basic spawn and wait", "[async][runtime]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};
    auto counter = AsyncTestState<int>::create(0);

    auto future = runtime.spawn(pingPongTask(&runtime, counter));
    auto const result = future.get();

    REQUIRE(result != std::this_thread::get_id());
    REQUIRE(counter.get() == 2);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("Async runtime - Exception handling", "[async][runtime]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};

    // Logging version - should not crash
    REQUIRE_NOTHROW(runtime.spawnLogged(failingTask(&runtime)));

    // Future version - should throw when getting result
    auto future = runtime.spawn(failingTask(&runtime));
    REQUIRE_THROWS_AS(future.get(), std::runtime_error);

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
