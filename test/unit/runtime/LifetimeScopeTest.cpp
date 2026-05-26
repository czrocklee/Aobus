// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include <ao/rt/ImmediateControlExecutor.h>
#include <ao/rt/async/LifetimeScope.h>
#include <ao/rt/async/Runtime.h>
#include <ao/rt/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>
#include <utility>

namespace ao::rt::test
{
  using namespace ao::rt::async;

  namespace
  {
    struct MemberTaskOwner final
    {
      MemberTaskOwner(Runtime& runtime, LifetimeScope& scope, AsyncTestState<int> completed)
        : runtime{runtime}, completed{std::move(completed)}
      {
        runtime.spawnWithLifetime(&scope, run());
      }

      Task<void> run()
      {
        co_await runtime.resumeOnWorker();
        (*completed)++;
      }

      Runtime& runtime;
      AsyncTestState<int> completed;
    };

    Task<void> longRunningTask(Runtime* runtime, AsyncBarrier* barrier, AsyncTestState<bool> completed)
    {
      co_await runtime->resumeOnWorker();
      barrier->wait(); // deterministic wait point (blocks worker thread)

      co_await runtime->resumeOnControl();
      // If cancelled, this line should never be reached.
      completed.set(true);
    }
  }

  TEST_CASE("LifetimeScope - Completion without cancellation", "[async][unit][runtime]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};
    auto barrier = AsyncBarrier{};
    auto completed = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(&scope, longRunningTask(&runtime, &barrier, completed));

      barrier.release();

      // Wait for it to complete on the worker thread/ImmediateControlExecutor
      REQUIRE(completed.waitUntil(true));
    }

    REQUIRE(completed.get());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - Automatic cancellation", "[async][unit][runtime]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};
    auto barrier = AsyncBarrier{};
    auto completed = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(&scope, longRunningTask(&runtime, &barrier, completed));

      // Destroy scope while task is blocked at the barrier
    }

// Now release the barrier - task should resume but be cancelled at resumeOnControl
    barrier.release();

    // Yield a bounded number of times to let the worker thread process the cancellation.
    // The barrier already guarantees ordering; yield merely assists thread scheduling.
    for (std::int32_t i = 0; i < 32; ++i)
    {
      std::this_thread::yield();
    }

    REQUIRE_FALSE(completed.get());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - Member task lifecycle", "[async][unit][runtime]")
  {
    auto executor = ImmediateControlExecutor{};
    auto runtime = Runtime{executor};
    auto completed = AsyncTestState<int>::create(0);

    {
      auto scope = LifetimeScope{};
      auto owner = MemberTaskOwner{runtime, scope, completed};

      // Coroutine finishes while owner and scope are alive
      REQUIRE(completed.waitUntil(1));
    }

    REQUIRE(completed.get() == 1);
    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
