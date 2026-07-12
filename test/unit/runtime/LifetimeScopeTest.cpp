// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/async/ImmediateExecutor.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <barrier>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <stop_token>
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
        runtime.spawnWithLifetime(&scope, [this](std::stop_token const stopToken) { return run(stopToken); });
      }

      Task<void> run(std::stop_token const stopToken)
      {
        co_await runtime.resumeOnWorker(stopToken);
        completed.increment();
      }

      Runtime& runtime;
      AsyncTestState<int> completed;
    };

    struct TaskExitObserver final
    {
      AsyncTestState<bool> exited;

      explicit TaskExitObserver(AsyncTestState<bool> exited)
        : exited{std::move(exited)}
      {
      }

      ~TaskExitObserver() { exited.set(true); }

      TaskExitObserver(TaskExitObserver const&) = delete;
      TaskExitObserver(TaskExitObserver&&) = delete;
      TaskExitObserver& operator=(TaskExitObserver const&) = delete;
      TaskExitObserver& operator=(TaskExitObserver&&) = delete;
    };

    Task<void> longRunningTask(Runtime* runtime,
                               AsyncBarrier* barrier,
                               AsyncTestState<bool> reachedBarrierWait,
                               AsyncTestState<bool> reachedCallbackHop,
                               AsyncTestState<bool> completed,
                               AsyncTestState<bool> taskExited,
                               std::stop_token const stopToken)
    {
      auto exitObserver = TaskExitObserver{taskExited};

      co_await runtime->resumeOnWorker(stopToken);
      reachedBarrierWait.set(true);
      barrier->wait(); // deterministic wait point (blocks worker thread)

      reachedCallbackHop.set(true);
      co_await runtime->resumeOnCallbackExecutor(stopToken);
      // If cancelled, this line should never be reached.
      completed.set(true);
    }

    Task<void> pendingControlResumeTask(Runtime* runtime,
                                        AsyncTestState<bool> completed,
                                        std::stop_token const stopToken = {})
    {
      co_await runtime->resumeOnWorker(stopToken);
      co_await runtime->resumeOnCallbackExecutor(stopToken);
      completed.set(true);
    }

    Task<void> racingSleep(Runtime* runtime, AsyncTestState<bool> taskExited, std::stop_token const stopToken)
    {
      auto exitObserver = TaskExitObserver{taskExited};
      co_await runtime->sleepFor(std::chrono::hours{1}, stopToken);
    }
  } // namespace

  TEST_CASE("LifetimeScope - task completes while scope remains alive", "[runtime][unit][async][lifetime]")
  {
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor};
    auto barrier = AsyncBarrier{};
    auto reachedBarrierWait = AsyncTestState<bool>::create(false);
    auto reachedCallbackHop = AsyncTestState<bool>::create(false);
    auto completed = AsyncTestState<bool>::create(false);
    auto taskExited = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(
        &scope,
        [&](std::stop_token const stopToken)
        {
          return longRunningTask(
            &runtime, &barrier, reachedBarrierWait, reachedCallbackHop, completed, taskExited, stopToken);
        });

      REQUIRE(reachedBarrierWait.waitUntil(true));
      barrier.release();
      executor.checkQueued();
      CHECK(reachedCallbackHop.load());
      CHECK_FALSE(completed.load());
      CHECK_FALSE(taskExited.load());

      executor.runUntilIdle();
      CHECK(completed.load());
      CHECK(taskExited.load());
    }

    CHECK(completed.load());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - destruction cancels blocked task before callback resume",
            "[runtime][unit][lifetime][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor};
    auto barrier = AsyncBarrier{};
    auto reachedBarrierWait = AsyncTestState<bool>::create(false);
    auto reachedCallbackHop = AsyncTestState<bool>::create(false);
    auto completed = AsyncTestState<bool>::create(false);
    auto taskExited = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(
        &scope,
        [&](std::stop_token const stopToken)
        {
          return longRunningTask(
            &runtime, &barrier, reachedBarrierWait, reachedCallbackHop, completed, taskExited, stopToken);
        });

      REQUIRE(reachedBarrierWait.waitUntil(true));
      // Destroy scope while task is blocked at the barrier.
    }

    barrier.release();
    REQUIRE(reachedCallbackHop.waitUntil(true));
    REQUIRE(taskExited.waitUntil(true));

    CHECK_FALSE(completed.load());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - cancellation before queued callback resume prevents completion",
            "[runtime][unit][lifetime][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor};
    auto completed = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(&scope,
                                [&runtime, completed](std::stop_token const stopToken)
                                { return pendingControlResumeTask(&runtime, completed, stopToken); });
      executor.checkQueued();
    }

    executor.runUntilIdle();

    CHECK_FALSE(completed.load());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("OperationCancelled - current exception guard recognizes cancellation",
            "[runtime][unit][async][cancellation]")
  {
    CHECK_THROWS_AS(
      []
      {
        try
        {
          std::rethrow_exception(std::make_exception_ptr(OperationCancelled{}));
        }
        catch (...)
        {
          rethrowIfOperationCancelled();
        }
      }(),
      OperationCancelled);

    CHECK_THROWS_AS(
      []
      {
        try
        {
          std::rethrow_exception(
            std::make_exception_ptr(boost::system::system_error{boost::asio::error::operation_aborted}));
        }
        catch (...)
        {
          rethrowIfOperationCancelled();
        }
      }(),
      OperationCancelled);

    CHECK_NOTHROW(
      []
      {
        try
        {
          std::rethrow_exception(std::make_exception_ptr(std::runtime_error{"not cancellation"}));
        }
        catch (...)
        {
          rethrowIfOperationCancelled();
        }
      }());
  }

  TEST_CASE("LifetimeScope - member task can complete while owner remains alive", "[runtime][unit][async][lifetime]")
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

    CHECK(completed.load() == 1);
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - cancellation races safely with concurrent completion",
            "[runtime][regression][lifetime][concurrency]")
  {
    constexpr std::size_t kIterations = 64;
    auto executor = ManualExecutor{};
    auto sleeper = ControlledSleeper{};
    auto runtime = Runtime{executor, 4, &sleeper};

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration)
    {
      auto taskExited = AsyncTestState<bool>::create(false);
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(&scope,
                                [&runtime, taskExited](std::stop_token const stopToken)
                                { return racingSleep(&runtime, taskExited, stopToken); });
      REQUIRE(sleeper.waitForCallCount(iteration + 1));
      auto const sleepId = sleeper.call(iteration).id;
      auto start = std::barrier{2};
      bool completionDispatched = false;

      {
        auto completionThread = std::jthread{[&]
                                             {
                                               start.arrive_and_wait();
                                               completionDispatched = sleeper.fireById(sleepId);
                                             }};
        start.arrive_and_wait();
        scope.cancelAll();
      }

      CHECK((completionDispatched || sleeper.call(iteration).cancelled));
      REQUIRE(taskExited.waitUntil(true));
    }

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
