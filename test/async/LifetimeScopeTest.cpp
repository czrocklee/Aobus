// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/LifetimeScope.h>

#include "test/runtime/AsyncTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string_view>
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
        runtime.spawnWithLifetime(scope, [this](std::stop_token const stopToken) { return runAsync(stopToken); });
      }

      Task<void> runAsync(std::stop_token const stopToken)
      {
        co_await runtime.resumeOnWorkerAsync(stopToken);
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

    Task<void> longRunningTaskAsync(Runtime* runtime,
                                    AsyncBarrier* barrier,
                                    AsyncTestState<bool> reachedBarrierWait,
                                    AsyncTestState<bool> reachedCallbackHop,
                                    AsyncTestState<bool> completed,
                                    AsyncTestState<bool> taskExited,
                                    std::stop_token const stopToken)
    {
      auto exitObserver = TaskExitObserver{taskExited};

      co_await runtime->resumeOnWorkerAsync(stopToken);
      reachedBarrierWait.set(true);
      barrier->wait(); // deterministic wait point (blocks worker thread)

      reachedCallbackHop.set(true);
      co_await runtime->resumeOnCallbackExecutorAsync(stopToken);
      // If cancelled, this line should never be reached.
      completed.set(true);
    }

    Task<void> pendingControlResumeTaskAsync(Runtime* runtime,
                                             AsyncTestState<bool> completed,
                                             std::stop_token const stopToken = {})
    {
      co_await runtime->resumeOnWorkerAsync(stopToken);
      co_await runtime->resumeOnCallbackExecutorAsync(stopToken);
      completed.set(true);
    }

    enum class SleepOutcome : std::uint8_t
    {
      Expired,
      Cancelled
    };

    Task<void> racingSleepAsync(Runtime* runtime, std::promise<SleepOutcome> outcome, std::stop_token const stopToken)
    {
      try
      {
        co_await runtime->sleepForAsync(std::chrono::hours{1}, stopToken);
        outcome.set_value(SleepOutcome::Expired);
      }
      catch (OperationCancelled const&)
      {
        outcome.set_value(SleepOutcome::Cancelled);
      }
    }

    Task<void> noOpAsync()
    {
      co_return;
    }

    Task<bool> holdWorkerAsync(AsyncTestState<bool> entered, AsyncTestState<bool> release)
    {
      entered.set(true);
      // If cancellation incorrectly waits for the queued task, free the only
      // worker independently and report that the driver did not release it.
      co_return release.tryWaitUntil(true, std::chrono::seconds{30});
    }

    std::future<bool> occupyWorker(Runtime& runtime, AsyncTestState<bool> release)
    {
      auto entered = AsyncTestState<bool>::create(false);
      auto task = runtime.spawn(holdWorkerAsync(entered, std::move(release)));
      REQUIRE(entered.tryWaitUntil(true));
      return std::async(std::launch::async, [task = std::move(task)] mutable { return task.get(); });
    }
  } // namespace

  TEST_CASE("LifetimeScope - task completes while scope remains alive", "[concurrency]")
  {
    auto barrier = AsyncBarrier{};
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor};
    auto reachedBarrierWait = AsyncTestState<bool>::create(false);
    auto reachedCallbackHop = AsyncTestState<bool>::create(false);
    auto completed = AsyncTestState<bool>::create(false);
    auto taskExited = AsyncTestState<bool>::create(false);
    auto releaseBarrier = gsl_lite::finally([&barrier] { barrier.release(); });

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(
        scope,
        [&runtime, &barrier, reachedBarrierWait, reachedCallbackHop, completed, taskExited](
          std::stop_token const stopToken)
        {
          return longRunningTaskAsync(
            &runtime, &barrier, reachedBarrierWait, reachedCallbackHop, completed, taskExited, stopToken);
        });

      REQUIRE(reachedBarrierWait.tryWaitUntil(true));
      barrier.release();
      executor.checkQueued();
      CHECK(reachedCallbackHop.load());
      CHECK_FALSE(completed.load());
      CHECK_FALSE(taskExited.load());

      executor.runUntilIdle();
      CHECK(completed.load());
      CHECK(taskExited.load());

      // Drain the worker-side completion before checking that the open scope retired the task.
      runtime.join();
      CHECK(scope.empty());
    }

    CHECK(completed.load());
  }

  TEST_CASE("LifetimeScope - destruction cancels blocked task before callback resume", "[concurrency]")
  {
    auto barrier = AsyncBarrier{};
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor};
    auto reachedBarrierWait = AsyncTestState<bool>::create(false);
    auto reachedCallbackHop = AsyncTestState<bool>::create(false);
    auto completed = AsyncTestState<bool>::create(false);
    auto taskExited = AsyncTestState<bool>::create(false);
    auto releaseBarrier = gsl_lite::finally([&barrier] { barrier.release(); });

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(
        scope,
        [&runtime, &barrier, reachedBarrierWait, reachedCallbackHop, completed, taskExited](
          std::stop_token const stopToken)
        {
          return longRunningTaskAsync(
            &runtime, &barrier, reachedBarrierWait, reachedCallbackHop, completed, taskExited, stopToken);
        });

      REQUIRE(reachedBarrierWait.tryWaitUntil(true));
      // Destroy scope while task is blocked at the barrier.
    }

    barrier.release();
    REQUIRE(reachedCallbackHop.tryWaitUntil(true));
    REQUIRE(taskExited.tryWaitUntil(true));

    CHECK_FALSE(completed.load());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - cancellation before queued callback resume prevents completion", "[concurrency]")
  {
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor};
    auto completed = AsyncTestState<bool>::create(false);

    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(scope,
                                [&runtime, completed](std::stop_token const stopToken)
                                { return pendingControlResumeTaskAsync(&runtime, completed, stopToken); });
      executor.checkQueued();
    }

    executor.runUntilIdle();

    CHECK_FALSE(completed.load());
    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("LifetimeScope - a closed scope does not submit a task", "[concurrency]")
  {
    auto worker = std::future<bool>{};
    auto release = AsyncTestState<bool>::create(false);
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor, 1};
    auto scope = LifetimeScope{};
    auto cleanup = gsl_lite::finally([&release] { release.set(true); });
    worker = occupyWorker(runtime, release);
    scope.cancelAll();
    scope.cancelAll();

    auto sentinelPtr = std::make_shared<int>(0);
    auto weakSentinelPtr = std::weak_ptr<int>{sentinelPtr};
    auto factoryEntries = AsyncTestState<int>::create(0);
    runtime.spawnWithLifetime(scope,
                              [sentinelPtr = std::move(sentinelPtr), factoryEntries](std::stop_token)
                              {
                                factoryEntries.set(++*sentinelPtr);
                                return noOpAsync();
                              });

    // A queued coroutine would still own the factory even if already cancelled.
    CHECK(weakSentinelPtr.expired());
    CHECK(scope.empty());

    release.set(true);
    REQUIRE(worker.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    REQUIRE(worker.get());
    runtime.join();
    CHECK(factoryEntries.load() == 0);
  }

  TEST_CASE("Runtime - cancelling an admitted task before its worker turn skips the factory", "[concurrency]")
  {
    auto worker = std::future<bool>{};
    auto release = AsyncTestState<bool>::create(false);
    auto factoryEntered = AsyncTestState<bool>::create(false);
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor, 1};
    auto cleanup = gsl_lite::finally([&release] { release.set(true); });
    worker = occupyWorker(runtime, release);

    auto makeTask = [factoryEntered](std::stop_token)
    {
      factoryEntered.set(true);
      return noOpAsync();
    };

    SECTION("Resetting its handle")
    {
      auto handle = runtime.spawnCancellable(makeTask);
      handle.reset();
    }

    SECTION("Closing its scope")
    {
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(scope, makeTask);
      REQUIRE_FALSE(scope.empty());
      scope.cancelAll();
      CHECK(scope.empty());
    }

    release.set(true);
    REQUIRE(worker.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    REQUIRE(worker.get());
    runtime.join();
    CHECK_FALSE(factoryEntered.load());
  }

  TEST_CASE("LifetimeScope - a task factory can cancel its own scope before returning", "[concurrency]")
  {
    auto executor = ManualExecutor{};
    auto scope = LifetimeScope{};
    auto runtime = Runtime{executor, 1};
    auto cancellationCount = AsyncTestState<int>::create(0);
    auto spawnOnWorkerAsync =
      [](Runtime* runtime, LifetimeScope* scope, AsyncTestState<int> cancellationCount) -> Task<void>
    {
      runtime->spawnWithLifetime(*scope,
                                 [scope, cancellationCount](std::stop_token const stopToken)
                                 {
                                   auto onStop = std::stop_callback{stopToken,
                                                                    [scope, cancellationCount]
                                                                    {
                                                                      scope->cancelAll();
                                                                      cancellationCount.increment();
                                                                    }};
                                   scope->cancelAll();
                                   return noOpAsync();
                                 });
      co_return;
    };
    auto spawningTask = runtime.spawn(spawnOnWorkerAsync(&runtime, &scope, cancellationCount));
    spawningTask.get();
    runtime.join();

    CHECK(cancellationCount.load() == 1);
    CHECK(scope.empty());
  }

  TEST_CASE("OperationCancelled - current exception guard recognizes cancellation")
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

  TEST_CASE("OperationCancelled - exception pointer classification recognizes only cancellation")
  {
    CHECK_FALSE(isOperationCancelled(std::exception_ptr{}));
    CHECK(isOperationCancelled(std::make_exception_ptr(OperationCancelled{})));
    CHECK(isOperationCancelled(
      std::make_exception_ptr(boost::system::system_error{boost::asio::error::operation_aborted})));
    CHECK_FALSE(isOperationCancelled(std::make_exception_ptr(std::runtime_error{"not cancellation"})));
    CHECK_FALSE(isOperationCancelled(std::make_exception_ptr(42)));
  }

  TEST_CASE("OperationCancelled - exception rethrow normalizes cancellation")
  {
    bool deferredFailureRethrown = false;

    try
    {
      rethrowException(std::make_exception_ptr(std::runtime_error{"deferred failure"}));
    }
    catch (std::runtime_error const& error)
    {
      deferredFailureRethrown = true;
      CHECK(std::string_view{error.what()} == "deferred failure");
    }

    CHECK(deferredFailureRethrown);

    bool cancellationNormalized = false;

    try
    {
      rethrowException(std::make_exception_ptr(boost::system::system_error{boost::asio::error::operation_aborted}));
    }
    catch (OperationCancelled const&)
    {
      cancellationNormalized = true;
    }

    CHECK(cancellationNormalized);
  }

  TEST_CASE("LifetimeScope - member task can complete while owner remains alive", "[concurrency]")
  {
    auto ownerPtr = std::unique_ptr<MemberTaskOwner>{};
    auto executor = InlineExecutor{};
    auto runtime = Runtime{executor};
    auto completed = AsyncTestState<int>::create(0);

    {
      auto scope = LifetimeScope{};
      ownerPtr = std::make_unique<MemberTaskOwner>(runtime, scope, completed);

      // Coroutine finishes while owner and scope are alive
      REQUIRE(completed.tryWaitUntil(1));
    }

    CHECK(completed.load() == 1);
    runtime.requestStop();
    runtime.join();
    ownerPtr.reset();
  }

  TEST_CASE("LifetimeScope - cancellation races safely with concurrent completion", "[concurrency]")
  {
    constexpr std::size_t kIterations = 64;
    auto executor = ManualExecutor{};
    auto sleeper = ControlledSleeper{};
    auto runtime = Runtime{executor, 4, &sleeper};

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration)
    {
      auto outcome = std::promise<SleepOutcome>{};
      auto result = outcome.get_future();
      auto scope = LifetimeScope{};
      runtime.spawnWithLifetime(scope,
                                [&runtime, outcome = std::move(outcome)](std::stop_token const stopToken) mutable
                                { return racingSleepAsync(&runtime, std::move(outcome), stopToken); });
      REQUIRE(sleeper.tryWaitForCallCount(iteration + 1));
      auto start = std::barrier{2};
      bool completionDispatched = false;

      {
        auto completionThread = std::jthread{[&]
                                             {
                                               start.arrive_and_wait();
                                               completionDispatched = sleeper.tryFire(iteration);
                                             }};
        start.arrive_and_wait();
        scope.cancelAll();
      }

      CHECK(completionDispatched != sleeper.call(iteration).cancelled);
      REQUIRE(result.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
      // A claimed expiry may still lose to Runtime's post-wait stop checkpoint.
      auto const terminal = result.get();
      CHECK((terminal == SleepOutcome::Cancelled || completionDispatched));
    }

    runtime.requestStop();
    runtime.join();
  }
} // namespace ao::rt::test
