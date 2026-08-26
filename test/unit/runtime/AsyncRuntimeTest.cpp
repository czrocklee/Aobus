// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/LoopExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace ao::rt::test
{
  using namespace ao::async;

  namespace
  {
    Task<std::thread::id> pingPongTask(Runtime* runtime, AsyncTestState<int> counter)
    {
      co_await runtime->resumeOnWorker();
      // Now on worker thread — the thread switch is the behavior under test.
      counter.increment();

      co_await runtime->resumeOnCallbackExecutor();
      // Now back on the callback executor's owner thread.
      counter.increment();

      co_return std::this_thread::get_id();
    }

    Task<void> callbackAfterRuntimeShutdown(Runtime* runtime,
                                            AsyncTestState<bool> resumed,
                                            [[maybe_unused]] std::shared_ptr<void> lifetimePtr)
    {
      co_await runtime->resumeOnCallbackExecutor();
      resumed.set(true);
    }

    Task<void> failingTask(Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      throw std::runtime_error{"Test failure"};
    }

    class NonDefaultTaskResult final
    {
    public:
      explicit NonDefaultTaskResult(std::int32_t value)
        : _value{value}
      {
      }

      NonDefaultTaskResult(NonDefaultTaskResult const&) = delete;
      NonDefaultTaskResult& operator=(NonDefaultTaskResult const&) = delete;
      NonDefaultTaskResult(NonDefaultTaskResult&&) noexcept = default;
      NonDefaultTaskResult& operator=(NonDefaultTaskResult&&) = delete;
      ~NonDefaultTaskResult() = default;

      std::int32_t value() const noexcept { return _value; }

    private:
      std::int32_t _value;
    };

    class ThrowingDefaultTaskResult final
    {
    public:
      ThrowingDefaultTaskResult() { throw std::runtime_error{"Transport attempted default construction"}; }

      explicit ThrowingDefaultTaskResult(std::int32_t value) noexcept
        : _value{value}
      {
      }

      std::int32_t value() const noexcept { return _value; }

    private:
      std::int32_t _value = 0;
    };

    Task<NonDefaultTaskResult> nonDefaultResultTask(Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      co_return NonDefaultTaskResult{42};
    }

    [[noreturn]] NonDefaultTaskResult throwNonDefaultResultFailure()
    {
      throw std::runtime_error{"Non-default result failure"};
    }

    Task<NonDefaultTaskResult> failingNonDefaultResultTask(Runtime* runtime)
    {
      co_await runtime->resumeOnWorker();
      co_return throwNonDefaultResultFailure();
    }

    Task<ThrowingDefaultTaskResult> throwingDefaultResultTask(Runtime* runtime, bool fail)
    {
      co_await runtime->resumeOnWorker();

      if (fail)
      {
        throw std::runtime_error{"Original task failure"};
      }

      co_return ThrowingDefaultTaskResult{84};
    }

    Task<void> sleepAndRecord(Runtime* runtime,
                              std::chrono::milliseconds const delay,
                              AsyncTestState<std::uint32_t> callbackCount,
                              AsyncTestState<bool> ranOnWorker,
                              std::stop_token const stopToken)
    {
      co_await runtime->sleepFor(delay, stopToken);
      ranOnWorker.set(!runtime->callbackExecutor().isCurrent());
      callbackCount.increment();
    }

    class TaskExitRecorder final
    {
    public:
      explicit TaskExitRecorder(AsyncTestState<std::uint32_t> exitCount)
        : _exitCount{std::move(exitCount)}
      {
      }

      ~TaskExitRecorder() { _exitCount.increment(); }

      TaskExitRecorder(TaskExitRecorder const&) = delete;
      TaskExitRecorder& operator=(TaskExitRecorder const&) = delete;
      TaskExitRecorder(TaskExitRecorder&&) = delete;
      TaskExitRecorder& operator=(TaskExitRecorder&&) = delete;

    private:
      AsyncTestState<std::uint32_t> _exitCount;
    };

    Task<void> timedCancellationRace(Runtime* runtime,
                                     AsyncTestState<std::uint32_t> startedCount,
                                     AsyncTestState<std::uint32_t> exitCount,
                                     std::stop_token const stopToken)
    {
      auto const exitRecorder = TaskExitRecorder{exitCount};
      startedCount.increment();
      co_await runtime->sleepFor(std::chrono::milliseconds{1}, stopToken);
    }
  } // namespace

  TEST_CASE("AsyncRuntime - ready task eagerly owns and returns its value", "[runtime][unit][async]")
  {
    auto executor = InlineExecutor{};
    auto runtime = Runtime{executor};
    auto valuePtr = std::make_unique<std::int32_t>(42);
    auto future = runtime.spawn(makeReadyTask(std::move(valuePtr)));
    CHECK_FALSE(valuePtr);
    CHECK(*future.get() == 42);
  }

  TEST_CASE("AsyncRuntime - spawn switches to worker and returns through callback executor",
            "[runtime][unit][async][concurrency]")
  {
    auto executor = LoopExecutor{};
    auto runtime = Runtime{executor};
    auto counter = AsyncTestState<int>::create(0);
    auto const ownerThread = std::this_thread::get_id();

    auto future = runtime.spawn(pingPongTask(&runtime, counter));
    executor.runOneTurn();
    auto const result = future.get();

    CHECK(result == ownerThread);
    CHECK(counter.load() == 2);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("AsyncRuntime - teardown discards queued callback and destroys its suspended frame",
            "[runtime][regression][async][concurrency]")
  {
    auto executor = QueuedExecutor{};
    auto resumed = AsyncTestState<bool>::create(false);
    auto lifetimePtr = std::make_shared<std::uint8_t>(0);
    auto const weakLifetimePtr = std::weak_ptr<void>{lifetimePtr};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);

    {
      auto runtimePtr = std::make_unique<Runtime>(executor, 1);
      runtimePtr->spawnLogged(
        flagCompletion(completedPtr, callbackAfterRuntimeShutdown(runtimePtr.get(), resumed, lifetimePtr)));
      lifetimePtr.reset();
      executor.checkQueued();
      CHECK_FALSE(weakLifetimePtr.expired());
      CHECK_FALSE(completedPtr->load());
    }

    CHECK_FALSE(weakLifetimePtr.expired());
    CHECK_FALSE(completedPtr->load());
    executor.drain();
    CHECK_FALSE(resumed.load());
    CHECK(completedPtr->load());
    CHECK(weakLifetimePtr.expired());
  }

  TEST_CASE("AsyncRuntime - terminal stop closes queued callback admission before destruction",
            "[runtime][regression][async][concurrency]")
  {
    auto executor = QueuedExecutor{};
    auto resumed = AsyncTestState<bool>::create(false);
    auto lifetimePtr = std::make_shared<std::uint8_t>(0);
    auto const weakLifetimePtr = std::weak_ptr<void>{lifetimePtr};
    auto runtimePtr = std::make_unique<Runtime>(executor, 1);

    runtimePtr->spawnLogged(callbackAfterRuntimeShutdown(runtimePtr.get(), resumed, lifetimePtr));
    lifetimePtr.reset();
    executor.checkQueued();

    runtimePtr->requestStop();
    runtimePtr->join();
    executor.drain();

    CHECK_FALSE(resumed.load());
    runtimePtr.reset();
    CHECK(weakLifetimePtr.expired());
  }

  TEST_CASE("AsyncRuntime - future task failure remains caller-owned", "[runtime][unit][async]")
  {
    auto executor = InlineExecutor{};
    auto runtime = Runtime{executor};

    auto future = runtime.spawn(failingTask(&runtime));
    REQUIRE_THROWS_AS(future.get(), std::runtime_error);

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("AsyncRuntime - spawn transports non-default-constructible and non-assignable results",
            "[runtime][unit][async]")
  {
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<NonDefaultTaskResult>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<NonDefaultTaskResult>);

    auto executor = InlineExecutor{};
    auto runtime = Runtime{executor};

    CHECK(runtime.spawn(nonDefaultResultTask(&runtime)).get().value() == 42);
    CHECK_THROWS_AS(runtime.spawn(failingNonDefaultResultTask(&runtime)).get(), std::runtime_error);
    CHECK(runtime.spawn(throwingDefaultResultTask(&runtime, false)).get().value() == 84);

    auto originalFailureFuture = runtime.spawn(throwingDefaultResultTask(&runtime, true));
    auto const originalFailure = captureTaskFutureException(originalFailureFuture);
    REQUIRE(originalFailure);

    try
    {
      std::rethrow_exception(originalFailure);
    }
    catch (std::runtime_error const& error)
    {
      CHECK(std::string_view{error.what()} == "Original task failure");
    }

    runtime.requestStop();
    runtime.join();
  }

  TEST_CASE("AsyncRuntime - sleep resumes its coroutine on the worker executor", "[runtime][unit][async]")
  {
    auto executor = QueuedExecutor{};
    auto runtime = Runtime{executor, 1};
    auto callbackCount = AsyncTestState<std::uint32_t>::create(0);
    auto ranOnWorker = AsyncTestState<bool>::create(false);

    auto task = runtime.spawnCancellable(
      [&runtime, callbackCount, ranOnWorker](std::stop_token const stopToken)
      { return sleepAndRecord(&runtime, std::chrono::milliseconds{1}, callbackCount, ranOnWorker, stopToken); });

    REQUIRE(callbackCount.waitUntil(1));
    CHECK(ranOnWorker.load());
    CHECK(executor.queuedCount() == 0);
  }

  TEST_CASE("AsyncRuntime - sleeping coroutine observes a thread-safe stop request",
            "[runtime][regression][async][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto sleeper = ControlledSleeper{};
    auto runtime = Runtime{executor, 1, &sleeper};
    auto callbackCount = AsyncTestState<std::uint32_t>::create(0);
    auto ranOnWorker = AsyncTestState<bool>::create(false);

    auto task = runtime.spawnCancellable(
      [&runtime, callbackCount, ranOnWorker](std::stop_token const stopToken)
      { return sleepAndRecord(&runtime, std::chrono::seconds{30}, callbackCount, ranOnWorker, stopToken); });
    REQUIRE(sleeper.waitForCallCount(1));
    auto const sleepingCall = sleeper.call(0);
    auto const cancellingThread = std::this_thread::get_id();

    task.reset();
    REQUIRE(sleeper.waitForCancellation(0));
    auto const cancelledCall = sleeper.call(0);
    CHECK(cancelledCall.cancelled);
    CHECK(cancelledCall.startedOn != cancellingThread);
    CHECK(cancelledCall.cancelledOn == cancellingThread);
    CHECK_FALSE(sleeper.fireById(sleepingCall.id));
    runtime.requestStop();
    runtime.join();

    CHECK(callbackCount.load() == 0);
  }

  TEST_CASE("AsyncRuntime - timer expiry races safely with cancellation",
            "[runtime][regression][async][concurrency][stress]")
  {
    constexpr std::uint32_t kIterationCount = 64;
    auto executor = ManualExecutor{};
    auto runtime = Runtime{executor, 4};
    auto startedCount = AsyncTestState<std::uint32_t>::create(0);
    auto exitCount = AsyncTestState<std::uint32_t>::create(0);

    for (std::uint32_t iteration = 0; iteration < kIterationCount; ++iteration)
    {
      auto task =
        runtime.spawnCancellable([&runtime, startedCount, exitCount](std::stop_token const stopToken)
                                 { return timedCancellationRace(&runtime, startedCount, exitCount, stopToken); });
      REQUIRE(startedCount.waitUntil(iteration + 1));

      auto cancellingThread = std::jthread{[task = std::move(task)] mutable
                                           {
                                             std::this_thread::sleep_for(std::chrono::milliseconds{1});
                                             task.reset();
                                           }};
      cancellingThread.join();
      REQUIRE(exitCount.waitUntil(iteration + 1));
    }

    runtime.requestStop();
    runtime.join();
    CHECK(exitCount.load() == kIterationCount);
  }
} // namespace ao::rt::test
