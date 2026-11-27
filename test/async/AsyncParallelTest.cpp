// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/runtime/AsyncTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::async;

  namespace
  {
    constexpr auto kFutureTimeout = std::chrono::seconds{5};

    template<typename T>
    concept HasPublicWorkerPool = requires(T& runtime) { runtime.workerPool(); };

    static_assert(!HasPublicWorkerPool<Runtime>);

    Task<> incrementTaskAsync(std::atomic<std::int32_t>* counter)
    {
      counter->fetch_add(1);
      co_return;
    }

    struct FirstChildFailure final : std::exception
    {};

    struct SecondChildFailure final : std::exception
    {};

    Task<> throwingTaskAsync(AsyncTestState<std::int32_t> started,
                             AsyncBarrier* release,
                             AsyncTestState<bool> aboutToThrow)
    {
      started.increment();
      release->wait();
      aboutToThrow.set(true);
      throw SecondChildFailure{};
      co_return;
    }

    Task<> gatedIncrementTaskAsync(AsyncTestState<std::int32_t> started,
                                   AsyncBarrier* release,
                                   std::atomic<std::int32_t>* counter,
                                   bool const fail)
    {
      started.increment();
      release->wait();
      counter->fetch_add(1);

      if (fail)
      {
        throw FirstChildFailure{};
      }

      co_return;
    }

    Task<> rendezvousTaskAsync(AsyncTestState<std::int32_t> started, AsyncBarrier* release)
    {
      started.increment();
      release->wait();
      co_return;
    }

    Task<> awaitAllTaskAsync(Runtime* runtime, std::vector<Task<>> tasks)
    {
      co_await runtime->whenAllAsync(std::move(tasks));
    }
  } // namespace

  TEST_CASE("whenAll - completes after all tasks ran", "[concurrency]")
  {
    auto executor = InlineExecutor{};
    auto counter = std::atomic<std::int32_t>{0};
    auto runtime = Runtime{executor, 4};

    auto tasks = std::vector<Task<>>{};

    for (std::int32_t index = 0; index < 8; ++index)
    {
      tasks.push_back(incrementTaskAsync(&counter));
    }

    runtime.spawn(awaitAllTaskAsync(&runtime, std::move(tasks))).get();

    CHECK(counter.load() == 8);
  }

  TEST_CASE("whenAll - accepts an empty task list")
  {
    auto executor = InlineExecutor{};
    auto runtime = Runtime{executor, 1};

    runtime.spawn(awaitAllTaskAsync(&runtime, {})).get();
  }

  TEST_CASE("whenAll - waits for every child and rethrows the first exception in task order", "[concurrency]")
  {
    auto deadline = ScopedTestDeadline{};
    auto executor = InlineExecutor{};
    auto counter = std::atomic<std::int32_t>{0};
    auto markerCount = std::atomic<std::int32_t>{0};
    auto started = AsyncTestState<std::int32_t>::create(0);
    auto aboutToThrow = AsyncTestState<bool>::create(false);
    auto finishRelease = AsyncBarrier{};
    auto throwRelease = AsyncBarrier{};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto markerResult = std::future<std::exception_ptr>{};
    auto result = std::future<std::exception_ptr>{};
    auto runtime = Runtime{executor, 2};
    auto cleanup = gsl_lite::finally(
      [&]
      {
        finishRelease.release();
        throwRelease.release();
      });

    bool firstChildFails = false;

    SECTION("The last child to finish succeeds")
    {
      firstChildFails = false;
    }

    SECTION("The last child to finish fails first in task order")
    {
      firstChildFails = true;
    }

    auto tasks = std::vector<Task<>>{};
    tasks.push_back(gatedIncrementTaskAsync(started, &finishRelease, &counter, firstChildFails));
    tasks.push_back(throwingTaskAsync(started, &throwRelease, aboutToThrow));
    auto future = runtime.spawn(flagCompletionAsync(completedPtr, awaitAllTaskAsync(&runtime, std::move(tasks))));

    REQUIRE(started.tryWaitUntil(2));
    throwRelease.release();
    REQUIRE(aboutToThrow.tryWaitUntil(true));

    // One worker remains blocked. This marker can finish on the other worker only
    // after Asio's dispatched child-completion stack has processed the exception.
    markerResult = readFutureException(runtime.spawn(incrementTaskAsync(&markerCount)));
    REQUIRE(markerResult.wait_for(kFutureTimeout) == std::future_status::ready);
    REQUIRE_FALSE(markerResult.get());
    CHECK(markerCount.load() == 1);
    CHECK_FALSE(completedPtr->load());
    CHECK(counter.load() == 0);

    finishRelease.release();
    result = readFutureException(std::move(future));
    REQUIRE(result.wait_for(kFutureTimeout) == std::future_status::ready);
    auto const failure = result.get();
    REQUIRE(failure);

    if (firstChildFails)
    {
      CHECK_THROWS_AS(std::rethrow_exception(failure), FirstChildFailure);
    }
    else
    {
      CHECK_THROWS_AS(std::rethrow_exception(failure), SecondChildFailure);
    }

    CHECK(completedPtr->load());
    CHECK(counter.load() == 1);
  }

  TEST_CASE("whenAll - tasks run concurrently on the worker pool", "[concurrency]")
  {
    auto deadline = ScopedTestDeadline{};
    auto executor = InlineExecutor{};
    auto started = AsyncTestState<std::int32_t>::create(0);
    auto release = AsyncBarrier{};
    auto result = std::future<std::exception_ptr>{};
    auto runtime = Runtime{executor, 2};
    // Release blocked workers before Runtime joins, including assertion unwinding.
    auto cleanup = gsl_lite::finally([&release] { release.release(); });

    auto tasks = std::vector<Task<>>{};
    tasks.push_back(rendezvousTaskAsync(started, &release));
    tasks.push_back(rendezvousTaskAsync(started, &release));

    auto future = runtime.spawn(awaitAllTaskAsync(&runtime, std::move(tasks)));
    REQUIRE(started.tryWaitUntil(2));
    release.release();
    result = readFutureException(std::move(future));
    REQUIRE(result.wait_for(kFutureTimeout) == std::future_status::ready);
    CHECK_FALSE(result.get());
  }

  TEST_CASE("whenAll - awaiting coroutine holds no pool thread", "[concurrency]")
  {
    // With a single-thread pool the coordinator must release its thread while
    // suspended in whenAllAsync; a blocking wait would deadlock here instead of
    // letting the tasks run sequentially. Even joining that worker would hang.
    auto deadline = ScopedTestDeadline{};
    auto executor = InlineExecutor{};
    auto counter = std::atomic<std::int32_t>{0};
    auto runtime = Runtime{executor, 1};

    auto tasks = std::vector<Task<>>{};
    tasks.push_back(incrementTaskAsync(&counter));
    tasks.push_back(incrementTaskAsync(&counter));

    runtime.spawn(awaitAllTaskAsync(&runtime, std::move(tasks))).get();

    CHECK(counter.load() == 2);
  }
} // namespace ao::rt::test
