// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/QueuedExecutor.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
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
      std::int32_t wakeCount() const noexcept { return _wakeCount; }

    private:
      void wake() override { ++_wakeCount; }

      void executeTask(std::move_only_function<void()>& task) override
      {
        if (task)
        {
          task();
        }
      }

      std::int32_t _wakeCount = 0;
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
} // namespace ao::async::test
