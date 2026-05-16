// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <runtime/ImmediateControlExecutor.h>
#include <runtime/async/LifetimeScope.h>
#include <runtime/async/Runtime.h>
#include <runtime/async/Task.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace ao::rt::async::test
{
  namespace
  {
    class MemberTaskOwner final
    {
    public:
      MemberTaskOwner(Runtime& runtime, LifetimeScope& scope, std::atomic<int>& completed)
        : _runtime{runtime}, _scope{scope}, _completed{completed}
      {
      }

      void start() { spawnWithLifetime(_runtime, _scope, run()); }

    private:
      Task<void> run()
      {
        co_await resumeOnWorker(_runtime);
        co_await resumeOnUi(_runtime);

        _completed.fetch_add(1);
      }

      Runtime& _runtime;
      LifetimeScope& _scope;
      std::atomic<int>& _completed;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    Task<void> longRunningTask(Runtime& runtime, std::atomic<bool>& completed)
    {
      co_await resumeOnWorker(runtime);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      co_await resumeOnUi(runtime);
      // If cancelled, this line should never be reached.
      completed.store(true);
    }
  } // namespace

  TEST_CASE("LifetimeScope - Cancels tasks on destruction", "[runtime][async]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};
    auto completed = std::atomic<bool>{false};

    {
      auto scope = LifetimeScope{};
      spawnWithLifetime(runtime, scope, longRunningTask(runtime, completed));

      // Scope is destroyed here, cancelling the task while it's sleeping.
    }

    // Wait a bit to ensure the task had time to wake up and attempt resumeOnUi
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // It should NOT have completed because the scope was destroyed.
    REQUIRE_FALSE(completed.load());

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - Completes task if scope remains alive", "[runtime][async]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};
    auto completed = std::atomic<bool>{false};

    {
      auto scope = LifetimeScope{};
      spawnWithLifetime(runtime, scope, longRunningTask(runtime, completed));

      // Keep scope alive until it completes
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    REQUIRE(completed.load());

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - Runs member coroutine after start returns", "[runtime][async]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};
    auto completed = std::atomic<int>{0};

    {
      auto scope = LifetimeScope{};
      auto owner = MemberTaskOwner{runtime, scope, completed};

      owner.start();

      for (int attempt = 0; attempt < 50 && completed.load() == 0; ++attempt)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }

    REQUIRE(completed.load() == 1);

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::async::test
