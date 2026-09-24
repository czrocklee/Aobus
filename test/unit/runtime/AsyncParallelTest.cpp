// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <catch2/catch_test_macros.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::async;

  namespace
  {
    template<typename T>
    concept HasPublicWorkerPool = requires(T& runtime) { runtime.workerPool(); };

    static_assert(!HasPublicWorkerPool<Runtime>);

    Task<> incrementTaskAsync(std::atomic<std::int32_t>* counter)
    {
      counter->fetch_add(1);
      co_return;
    }

    Task<> throwingTaskAsync(AsyncTestState<std::int32_t> started,
                             AsyncBarrier* release,
                             AsyncTestState<bool> aboutToThrow)
    {
      started.increment();
      release->wait();
      aboutToThrow.set(true);
      throw std::runtime_error{"whenAll test failure"};
      co_return;
    }

    Task<> gatedIncrementTaskAsync(AsyncTestState<std::int32_t> started,
                                   AsyncBarrier* release,
                                   std::atomic<std::int32_t>* counter)
    {
      started.increment();
      release->wait();
      counter->fetch_add(1);
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

  TEST_CASE("whenAll - completes after all tasks ran", "[runtime][unit][async][concurrency]")
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

  TEST_CASE("whenAll - empty task list completes immediately", "[runtime][unit][async]")
  {
    auto executor = InlineExecutor{};
    auto runtime = Runtime{executor, 1};

    runtime.spawn(awaitAllTaskAsync(&runtime, {})).get();
  }

  TEST_CASE("whenAll - rethrows a task exception after all tasks finished", "[runtime][unit][async][concurrency]")
  {
    auto executor = InlineExecutor{};
    auto counter = std::atomic<std::int32_t>{0};
    auto markerCount = std::atomic<std::int32_t>{0};
    auto started = AsyncTestState<std::int32_t>::create(0);
    auto aboutToThrow = AsyncTestState<bool>::create(false);
    auto finishRelease = AsyncBarrier{};
    auto throwRelease = AsyncBarrier{};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto runtime = Runtime{executor, 2};
    auto cleanup = gsl_lite::finally(
      [&]
      {
        finishRelease.release();
        throwRelease.release();
      });

    auto tasks = std::vector<Task<>>{};
    tasks.push_back(throwingTaskAsync(started, &throwRelease, aboutToThrow));
    tasks.push_back(gatedIncrementTaskAsync(started, &finishRelease, &counter));
    auto future = runtime.spawn(flagCompletionAsync(completedPtr, awaitAllTaskAsync(&runtime, std::move(tasks))));

    REQUIRE(started.tryWaitUntil(2));
    throwRelease.release();
    REQUIRE(aboutToThrow.tryWaitUntil(true));

    // One worker remains blocked. This marker can finish on the other worker only
    // after Asio's dispatched child-completion stack has processed the exception.
    runtime.spawn(incrementTaskAsync(&markerCount)).get();
    CHECK(markerCount.load() == 1);
    CHECK_FALSE(completedPtr->load());
    CHECK(counter.load() == 0);

    finishRelease.release();
    CHECK_THROWS_AS(future.get(), std::runtime_error);
    CHECK(completedPtr->load());
    CHECK(counter.load() == 1);
  }

  TEST_CASE("whenAll - tasks run concurrently on the worker pool", "[runtime][unit][async][concurrency]")
  {
    auto executor = InlineExecutor{};
    auto started = AsyncTestState<std::int32_t>::create(0);
    auto release = AsyncBarrier{};
    auto runtime = Runtime{executor, 2};
    // Release blocked workers before Runtime joins, including assertion unwinding.
    auto cleanup = gsl_lite::finally([&release] { release.release(); });

    auto tasks = std::vector<Task<>>{};
    tasks.push_back(rendezvousTaskAsync(started, &release));
    tasks.push_back(rendezvousTaskAsync(started, &release));

    auto future = runtime.spawn(awaitAllTaskAsync(&runtime, std::move(tasks)));
    auto const bothStarted = started.tryWaitUntil(2);
    release.release();
    future.get();

    CHECK(bothStarted);
  }

  TEST_CASE("whenAll - awaiting coroutine holds no pool thread", "[runtime][unit][async][concurrency]")
  {
    // With a single-thread pool the coordinator must release its thread while
    // suspended in whenAllAsync; a blocking wait would deadlock here instead of
    // letting the tasks run sequentially.
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
