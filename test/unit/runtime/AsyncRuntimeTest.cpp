// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <runtime/ImmediateControlExecutor.h>
#include <runtime/async/Runtime.h>
#include <runtime/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace ao::rt::async::test
{
  namespace
  {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    Task<std::thread::id> pingPongTask(Runtime& runtime, std::atomic<int>& counter)
    {
      co_await resumeOnWorker(runtime);
      // Now on worker thread
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      counter.fetch_add(1);

      co_await resumeOnUi(runtime);
      // Now back on UI (ImmediateControlExecutor for tests)
      counter.fetch_add(1);

      co_return std::this_thread::get_id();
    }

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    Task<void> failingTask(Runtime& runtime)
    {
      co_await resumeOnWorker(runtime);
      throw std::runtime_error{"Intentional failure"};
    }
  } // namespace

  TEST_CASE("Async Runtime - Basic Thread Switching", "[runtime][async]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};
    auto counter = std::atomic<int>{0};

    auto future = spawn(runtime, pingPongTask(runtime, counter));

    future.get(); // blocks until coroutine finishes

    REQUIRE(counter.load() == 2);
  }

  TEST_CASE("Async Runtime - Spawn Logged Exception Safety", "[runtime][async]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};

    REQUIRE_NOTHROW(spawnLogged(runtime, failingTask(runtime)));

    // Wait a bit to ensure the worker pool executes the failing task.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::async::test
