// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Exception.h>
#include <ao/async/QueuedExecutor.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <semaphore>
#include <thread>
#include <vector>

namespace ao::async::test
{
  namespace
  {
    class ManualQueuedExecutor final : public QueuedExecutorBase
    {
    public:
      void drain() { drainQueuedTasks(); }
      std::int32_t wakeCount() const noexcept { return _wakeCount.load(std::memory_order_relaxed); }

    private:
      void wake() override { _wakeCount.fetch_add(1, std::memory_order_relaxed); }

      void executeTask(std::move_only_function<void()>& task) override
      {
        if (task)
        {
          task();
        }
      }

      std::atomic<std::int32_t> _wakeCount = 0;
    };
  } // namespace

  TEST_CASE("QueuedExecutorBase - dispatch runs inline on the owner thread", "[runtime][unit][async]")
  {
    auto executor = ManualQueuedExecutor{};
    auto order = std::vector<int>{};

    executor.dispatch([&] { order.push_back(1); });
    executor.dispatch({});

    CHECK(order == std::vector<int>{1});
    CHECK(executor.wakeCount() == 0);
  }

  TEST_CASE("QueuedExecutorBase - dispatch from another thread queues and wakes", "[runtime][unit][async][concurrency]")
  {
    auto executor = ManualQueuedExecutor{};
    auto ran = std::atomic_bool{false};

    auto worker = std::jthread{[&] { executor.dispatch([&] { ran = true; }); }};
    worker.join();

    CHECK_FALSE(ran.load());
    CHECK(executor.wakeCount() == 1);

    executor.drain();

    CHECK(ran.load());
  }

  TEST_CASE("QueuedExecutorBase - defer always waits for a later executor turn", "[runtime][unit][async]")
  {
    auto executor = ManualQueuedExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer([&] { order.push_back(3); });
        order.push_back(2);
      });

    CHECK(order.empty());
    CHECK(executor.wakeCount() == 1);

    executor.drain();

    CHECK(order == std::vector<int>{1, 2});
    CHECK(executor.wakeCount() == 2);

    executor.drain();

    CHECK(order == std::vector<int>{1, 2, 3});
  }

  TEST_CASE("QueuedExecutorBase - a pending burst uses one wake and preserves FIFO order", "[runtime][unit][async]")
  {
    auto executor = ManualQueuedExecutor{};
    auto order = std::vector<int>{};

    for (std::int32_t value = 1; value <= 4; ++value)
    {
      executor.defer([&, value] { order.push_back(value); });
    }

    CHECK(executor.wakeCount() == 1);

    executor.drain();

    CHECK(order == std::vector<int>{1, 2, 3, 4});
  }

  TEST_CASE("QueuedExecutorBase - concurrent producers queue each task once with one wake",
            "[runtime][unit][async][concurrency]")
  {
    constexpr std::size_t kProducerCount = 8;
    auto executor = ManualQueuedExecutor{};
    auto startLine = std::barrier{static_cast<std::ptrdiff_t>(kProducerCount + 1)};
    auto executions = std::vector<int>(kProducerCount, 0);
    auto workers = std::vector<std::jthread>{};
    workers.reserve(kProducerCount);

    for (std::size_t producer = 0; producer < kProducerCount; ++producer)
    {
      workers.emplace_back(
        [&, producer]
        {
          startLine.arrive_and_wait();
          executor.defer([&, producer] { ++executions[producer]; });
        });
    }

    startLine.arrive_and_wait();

    for (auto& worker : workers)
    {
      worker.join();
    }

    CHECK(executor.wakeCount() == 1);

    executor.drain();

    CHECK(executions == std::vector<int>(kProducerCount, 1));
  }

  TEST_CASE("QueuedExecutorBase - tasks queued during a drain share one later wake",
            "[runtime][unit][async][concurrency]")
  {
    auto executor = ManualQueuedExecutor{};
    auto drainStarted = std::binary_semaphore{0};
    auto producerFinished = std::binary_semaphore{0};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        drainStarted.release();
        producerFinished.acquire();
        order.push_back(2);
      });

    auto producer = std::jthread{[&]
                                 {
                                   drainStarted.acquire();
                                   executor.defer([&] { order.push_back(3); });
                                   executor.defer([&] { order.push_back(4); });
                                   producerFinished.release();
                                 }};

    executor.drain();
    producer.join();

    CHECK(order == std::vector<int>{1, 2});
    CHECK(executor.wakeCount() == 2);

    executor.drain();

    CHECK(order == std::vector<int>{1, 2, 3, 4});
  }

  TEST_CASE("QueuedExecutorBase - a nested drain leaves deferred work for the next turn", "[runtime][unit][async]")
  {
    auto executor = ManualQueuedExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer([&] { order.push_back(3); });
        executor.drain();
        order.push_back(2);
      });

    executor.drain();

    CHECK(order == std::vector<int>{1, 2});
    CHECK(executor.wakeCount() == 2);

    executor.drain();

    CHECK(order == std::vector<int>{1, 2, 3});
  }

  TEST_CASE("QueuedExecutorBase - a failed drain still schedules newly deferred work", "[runtime][unit][async]")
  {
    auto executor = ManualQueuedExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer([&] { order.push_back(2); });
        throwException<Exception>("task failed");
      });

    CHECK_THROWS_AS(executor.drain(), Exception);
    CHECK(order == std::vector<int>{1});
    CHECK(executor.wakeCount() == 2);

    executor.drain();

    CHECK(order == std::vector<int>{1, 2});
  }
} // namespace ao::async::test
