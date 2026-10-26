// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/async/LoopExecutor.h>

#include <ao/async/QueuedExecutorBase.h>

#include <catch2/catch_test_macros.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <latch>
#include <semaphore>
#include <thread>
#include <vector>

namespace ao::async::test
{
  namespace
  {
    class FinalDrainExecutor final : public QueuedExecutorBase
    {
    public:
      void finish() { drainQueuedTasksUntilIdle(); }

    private:
      void wake() noexcept override {}
    };

    // Bounds a turn that should already be ready, so a lost wake fails instead of hanging.
    std::chrono::steady_clock::time_point turnDeadline()
    {
      return std::chrono::steady_clock::now() + std::chrono::seconds{5};
    }
  } // namespace

  TEST_CASE("LoopExecutor - owner dispatch runs inline", "[runtime][unit][async]")
  {
    auto executor = LoopExecutor{};
    auto order = std::vector<int>{};

    executor.dispatch([&] { order.push_back(1); });
    executor.dispatch({});

    CHECK(order == std::vector<int>{1});
    CHECK_FALSE(executor.tryRunReadyTurn());
  }

  TEST_CASE("LoopExecutor - foreign dispatch runs on the owner thread", "[runtime][unit][async][concurrency]")
  {
    auto executor = LoopExecutor{};
    auto const ownerThread = std::this_thread::get_id();
    auto callbackThread = std::thread::id{};

    auto worker = std::jthread{[&] { executor.dispatch([&] { callbackThread = std::this_thread::get_id(); }); }};
    worker.join();

    CHECK(callbackThread == std::thread::id{});

    REQUIRE(executor.tryRunOneTurnUntil(turnDeadline()));

    CHECK(callbackThread == ownerThread);
    CHECK_FALSE(executor.tryRunReadyTurn());
  }

  TEST_CASE("LoopExecutor - a timed-out wait preserves subsequent turns", "[runtime][unit][async][concurrency]")
  {
    auto executor = LoopExecutor{};

    SECTION("deadline is already expired")
    {
      CHECK_FALSE(executor.tryRunOneTurnUntil(std::chrono::steady_clock::now()));
    }

    SECTION("deadline lies ahead")
    {
      CHECK_FALSE(executor.tryRunOneTurnUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds{2}));
    }

    auto order = std::vector<int>{};
    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer([&] { order.push_back(2); });
      });

    REQUIRE(executor.tryRunOneTurnUntil(turnDeadline()));
    CHECK(order == std::vector<int>{1});
    REQUIRE(executor.tryRunReadyTurn());
    CHECK(order == std::vector<int>{1, 2});
    CHECK_FALSE(executor.tryRunReadyTurn());
  }

  TEST_CASE("LoopExecutor - a bounded wait accepts a foreign producer", "[runtime][unit][async][concurrency]")
  {
    auto executor = LoopExecutor{};
    auto const ownerThread = std::this_thread::get_id();
    auto callbackThread = std::thread::id{};
    auto startLine = std::barrier{2};
    auto worker = std::jthread{[&]
                               {
                                 startLine.arrive_and_wait();
                                 executor.defer([&] { callbackThread = std::this_thread::get_id(); });
                               }};
    startLine.arrive_and_wait();
    REQUIRE(executor.tryRunOneTurnUntil(turnDeadline()));
    CHECK(callbackThread == ownerThread);
    CHECK_FALSE(executor.tryRunReadyTurn());
  }

  TEST_CASE("LoopExecutor - concurrent producers share one ready turn", "[runtime][unit][async][concurrency]")
  {
    constexpr std::size_t kProducerCount = 8;
    auto executor = LoopExecutor{};
    auto startLine = std::latch{1};
    auto executions = std::vector<int>(kProducerCount, 0);
    auto workers = std::vector<std::jthread>{};
    workers.reserve(kProducerCount);
    // Unblock already-created workers if a later thread construction fails.
    auto const releaseWorkers = gsl_lite::finally(
      [&]
      {
        if (!startLine.try_wait())
        {
          startLine.count_down();
        }
      });

    for (std::size_t producer = 0; producer < kProducerCount; ++producer)
    {
      workers.emplace_back(
        [&, producer]
        {
          startLine.wait();
          executor.dispatch([&, producer] { ++executions[producer]; });
        });
    }

    startLine.count_down();

    for (auto& worker : workers)
    {
      worker.join();
    }

    REQUIRE(executor.tryRunOneTurnUntil(turnDeadline()));

    CHECK(executions == std::vector<int>(kProducerCount, 1));
    CHECK_FALSE(executor.tryRunReadyTurn());
  }

  TEST_CASE("LoopExecutor - a pending burst preserves FIFO order in one ready turn", "[runtime][unit][async]")
  {
    auto executor = LoopExecutor{};
    auto order = std::vector<std::int32_t>{};

    for (std::int32_t value = 1; value <= 4; ++value)
    {
      executor.defer([&, value] { order.push_back(value); });
    }

    REQUIRE(executor.tryRunOneTurnUntil(turnDeadline()));

    CHECK(order == std::vector<std::int32_t>{1, 2, 3, 4});
    CHECK_FALSE(executor.tryRunReadyTurn());
  }

  TEST_CASE("LoopExecutor - work queued during a turn runs in a later turn", "[runtime][unit][async][concurrency]")
  {
    auto executor = LoopExecutor{};
    auto drainStarted = std::binary_semaphore{0};
    auto producerFinished = std::binary_semaphore{0};
    auto order = std::vector<int>{};
    bool drainSignaled = false;
    bool producerFinishedInTurn = false;
    auto producerFailure = std::exception_ptr{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        drainSignaled = true;
        drainStarted.release();
        producerFinishedInTurn = producerFinished.try_acquire_for(std::chrono::seconds{5});
        order.push_back(2);
      });

    auto producer = std::jthread{[&]
                                 {
                                   auto const releaseOnExit = gsl_lite::finally([&] { producerFinished.release(); });
                                   drainStarted.acquire();

                                   try
                                   {
                                     executor.defer([&] { order.push_back(3); });
                                     executor.defer([&] { order.push_back(4); });
                                   }
                                   catch (...)
                                   {
                                     producerFailure = std::current_exception();
                                   }
                                 }};
    auto const releaseUnstartedProducer = gsl_lite::finally(
      [&]
      {
        if (!drainSignaled)
        {
          drainStarted.release();
        }
      });

    REQUIRE(executor.tryRunOneTurnUntil(turnDeadline()));
    producer.join();

    if (producerFailure)
    {
      std::rethrow_exception(producerFailure);
    }

    CHECK(producerFinishedInTurn);
    CHECK(order == std::vector<int>{1, 2});
    REQUIRE(executor.tryRunReadyTurn());
    CHECK(order == std::vector<int>{1, 2, 3, 4});
    CHECK_FALSE(executor.tryRunReadyTurn());
  }

  TEST_CASE("LoopExecutor - deferred work admitted during a turn runs later", "[runtime][unit][async]")
  {
    auto executor = LoopExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer([&] { order.push_back(3); });
        order.push_back(2);
      });

    REQUIRE(executor.tryRunOneTurnUntil(turnDeadline()));

    CHECK(order == std::vector<int>{1, 2});
    REQUIRE(executor.tryRunReadyTurn());
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK_FALSE(executor.tryRunReadyTurn());
  }

  TEST_CASE("LoopExecutor - nested pumping does not reenter a draining turn", "[runtime][unit][async]")
  {
    auto executor = LoopExecutor{};
    auto order = std::vector<int>{};
    bool nestedTurnRan = true;

    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer([&] { order.push_back(3); });
        nestedTurnRan = executor.tryRunReadyTurn();
        order.push_back(2);
      });

    REQUIRE(executor.tryRunOneTurnUntil(turnDeadline()));

    CHECK_FALSE(nestedTurnRan);
    CHECK(order == std::vector<int>{1, 2});
    REQUIRE(executor.tryRunReadyTurn());
    CHECK(order == std::vector<int>{1, 2, 3});
  }

  TEST_CASE("QueuedExecutorBase - final drain includes deferred continuations", "[runtime][unit][async]")
  {
    auto executor = FinalDrainExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer(
          [&]
          {
            order.push_back(2);
            executor.defer([&] { order.push_back(3); });
          });
      });

    executor.finish();

    CHECK(order == std::vector<int>{1, 2, 3});
    executor.finish();
    CHECK(order == std::vector<int>{1, 2, 3});
  }
} // namespace ao::async::test
