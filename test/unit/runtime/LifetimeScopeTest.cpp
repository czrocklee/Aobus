// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/async/Executor.h>
#include <ao/async/ImmediateExecutor.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace ao::rt::test
{
  using namespace ao::async;

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

    class QueuedExecutor final : public IExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }

      void dispatch(std::move_only_function<void()> task) override
      {
        auto const lock = std::scoped_lock{_mutex};
        _tasks.push_back(std::move(task));
      }

      void defer(std::move_only_function<void()> task) override { dispatch(std::move(task)); }

      void runQueued()
      {
        auto tasks = std::deque<std::move_only_function<void()>>{};

        {
          auto const lock = std::scoped_lock{_mutex};
          tasks.swap(_tasks);
        }

        while (!tasks.empty())
        {
          auto task = std::move(tasks.front());
          tasks.pop_front();
          task();
        }
      }

      bool waitUntilQueued(std::chrono::milliseconds timeout = std::chrono::milliseconds{500})
      {
        auto const start = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() - start < timeout)
        {
          {
            auto const lock = std::scoped_lock{_mutex};

            if (!_tasks.empty())
            {
              return true;
            }
          }

          std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        auto const lock = std::scoped_lock{_mutex};
        return !_tasks.empty();
      }

    private:
      std::deque<std::move_only_function<void()>> _tasks;
      std::mutex _mutex;
    };

    Task<void> longRunningTask(Runtime* runtime, AsyncBarrier* barrier, AsyncTestState<bool> completed)
    {
      co_await runtime->resumeOnWorker();
      barrier->wait(); // deterministic wait point (blocks worker thread)

      co_await runtime->resumeOnCallbackExecutor();
      // If cancelled, this line should never be reached.
      completed.set(true);
    }

    Task<void> pendingControlResumeTask(Runtime* runtime, AsyncTestState<bool> completed)
    {
      co_await runtime->resumeOnWorker();
      co_await runtime->resumeOnCallbackExecutor();
      completed.set(true);
    }
  }

  TEST_CASE("LifetimeScope - Completion without cancellation", "[async][unit][runtime]")
  {
    auto executor = ImmediateExecutor{};
    auto runtime = Runtime{executor};
    auto barrier = AsyncBarrier{};
    auto completed = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(&scope, longRunningTask(&runtime, &barrier, completed));

      barrier.release();

      // Wait for it to complete on the worker thread/ImmediateExecutor
      REQUIRE(completed.waitUntil(true));
    }

    REQUIRE(completed.get());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - Automatic cancellation", "[async][unit][runtime]")
  {
    auto executor = ImmediateExecutor{};
    auto runtime = Runtime{executor};
    auto barrier = AsyncBarrier{};
    auto completed = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(&scope, longRunningTask(&runtime, &barrier, completed));

      // Destroy scope while task is blocked at the barrier
    }

    // Now release the barrier - task should resume but be cancelled at resumeOnCallbackExecutor
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

  TEST_CASE("LifetimeScope - Cancellation before queued control resume", "[async][unit][runtime]")
  {
    auto executor = QueuedExecutor{};
    auto runtime = Runtime{executor};
    auto completed = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(&scope, pendingControlResumeTask(&runtime, completed));
      REQUIRE(executor.waitUntilQueued());
    }

    executor.runQueued();

    REQUIRE_FALSE(completed.get());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - Member task lifecycle", "[async][unit][runtime]")
  {
    auto executor = ImmediateExecutor{};
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
