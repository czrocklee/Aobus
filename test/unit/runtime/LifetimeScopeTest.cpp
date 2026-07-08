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

#include <exception>
#include <stdexcept>
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
                               AsyncTestState<bool> taskExited)
    {
      auto exitObserver = TaskExitObserver{taskExited};

      co_await runtime->resumeOnWorker();
      reachedBarrierWait.set(true);
      barrier->wait(); // deterministic wait point (blocks worker thread)

      reachedCallbackHop.set(true);
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
        &scope, longRunningTask(&runtime, &barrier, reachedBarrierWait, reachedCallbackHop, completed, taskExited));

      REQUIRE(reachedBarrierWait.waitUntil(true));
      barrier.release();
      executor.expectQueued();
      CHECK(reachedCallbackHop.get());
      CHECK_FALSE(completed.get());
      CHECK_FALSE(taskExited.get());

      executor.runUntilIdle();
      CHECK(completed.get());
      CHECK(taskExited.get());
    }

    CHECK(completed.get());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - destruction cancels blocked task before callback resume",
            "[runtime][unit][async][lifetime]")
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
        &scope, longRunningTask(&runtime, &barrier, reachedBarrierWait, reachedCallbackHop, completed, taskExited));

      REQUIRE(reachedBarrierWait.waitUntil(true));
      // Destroy scope while task is blocked at the barrier.
    }

    barrier.release();
    REQUIRE(reachedCallbackHop.waitUntil(true));
    REQUIRE(taskExited.waitUntil(true));

    CHECK_FALSE(completed.get());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - cancellation before queued callback resume prevents completion",
            "[runtime][unit][async][lifetime]")
  {
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor};
    auto completed = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(&scope, pendingControlResumeTask(&runtime, completed));
      executor.expectQueued();
    }

    executor.runUntilIdle();

    CHECK_FALSE(completed.get());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("Runtime - cancellation checkpoint reports OperationCancelled", "[runtime][unit][async][cancellation]")
  {
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor};
    auto completed = AsyncTestState<bool>::create(false);
    auto sawCancellation = AsyncTestState<bool>::create(false);
    auto signal = CancellationSignal{};

    runtime.spawn(pendingControlResumeTask(&runtime, completed),
                  signal.slot(),
                  [sawCancellation](std::exception_ptr exPtr) mutable
                  {
                    try
                    {
                      if (exPtr)
                      {
                        std::rethrow_exception(exPtr);
                      }
                    }
                    catch (OperationCancelled const&)
                    {
                      sawCancellation.set(true);
                    }
                  });

    executor.expectQueued();
    signal.emit(CancellationType::all);
    executor.runUntilIdle();

    REQUIRE(sawCancellation.waitUntil(true));
    CHECK_FALSE(completed.get());

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

    CHECK(completed.get() == 1);
    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
