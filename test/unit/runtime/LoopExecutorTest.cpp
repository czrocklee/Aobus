// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Exception.h>
#include <ao/async/LoopExecutor.h>

#include <catch2/catch_test_macros.hpp>

#include <barrier>
#include <cstddef>
#include <cstdint>
#include <semaphore>
#include <thread>
#include <vector>

namespace ao::async::test
{
  TEST_CASE("LoopExecutor - owner dispatch runs inline", "[runtime][unit][async]")
  {
    auto executor = LoopExecutor{};
    auto order = std::vector<int>{};

    executor.dispatch([&] { order.push_back(1); });
    executor.dispatch({});

    CHECK(order == std::vector<int>{1});
    CHECK_FALSE(executor.runReadyTurn());
  }

  TEST_CASE("LoopExecutor - foreign dispatch runs on the owner thread", "[runtime][unit][async][concurrency]")
  {
    auto executor = LoopExecutor{};
    auto const ownerThread = std::this_thread::get_id();
    auto callbackThread = std::thread::id{};

    auto worker = std::jthread{[&] { executor.dispatch([&] { callbackThread = std::this_thread::get_id(); }); }};
    worker.join();

    CHECK(callbackThread == std::thread::id{});

    executor.runOneTurn();

    CHECK(callbackThread == ownerThread);
    CHECK_FALSE(executor.runReadyTurn());
  }

  TEST_CASE("LoopExecutor - concurrent producers share one ready turn", "[runtime][unit][async][concurrency]")
  {
    constexpr std::size_t kProducerCount = 8;
    auto executor = LoopExecutor{};
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
          executor.dispatch([&, producer] { ++executions[producer]; });
        });
    }

    startLine.arrive_and_wait();

    for (auto& worker : workers)
    {
      worker.join();
    }

    executor.runOneTurn();

    CHECK(executions == std::vector<int>(kProducerCount, 1));
    CHECK_FALSE(executor.runReadyTurn());
  }

  TEST_CASE("LoopExecutor - a pending burst preserves FIFO order in one ready turn", "[runtime][unit][async]")
  {
    auto executor = LoopExecutor{};
    auto order = std::vector<std::int32_t>{};

    for (std::int32_t value = 1; value <= 4; ++value)
    {
      executor.defer([&, value] { order.push_back(value); });
    }

    executor.runOneTurn();

    CHECK(order == std::vector<std::int32_t>{1, 2, 3, 4});
    CHECK_FALSE(executor.runReadyTurn());
  }

  TEST_CASE("LoopExecutor - work queued during a turn runs in a later turn", "[runtime][unit][async][concurrency]")
  {
    auto executor = LoopExecutor{};
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

    executor.runOneTurn();
    producer.join();

    CHECK(order == std::vector<int>{1, 2});
    REQUIRE(executor.runReadyTurn());
    CHECK(order == std::vector<int>{1, 2, 3, 4});
    CHECK_FALSE(executor.runReadyTurn());
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

    executor.runOneTurn();

    CHECK(order == std::vector<int>{1, 2});
    REQUIRE(executor.runReadyTurn());
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK_FALSE(executor.runReadyTurn());
  }

  TEST_CASE("LoopExecutor - nested pumping does not reenter a draining turn", "[runtime][unit][async]")
  {
    auto executor = LoopExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        executor.defer([&] { order.push_back(3); });
        CHECK_FALSE(executor.runReadyTurn());
        order.push_back(2);
      });

    executor.runOneTurn();

    CHECK(order == std::vector<int>{1, 2});
    REQUIRE(executor.runReadyTurn());
    CHECK(order == std::vector<int>{1, 2, 3});
  }

  TEST_CASE("LoopExecutor - failed turn leaves newly deferred work ready", "[runtime][unit][async]")
  {
    auto executor = LoopExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        executor.defer([&] { order.push_back(2); });
        throwException<Exception>("task failed");
      });

    CHECK_THROWS_AS(executor.runOneTurn(), Exception);
    REQUIRE(executor.runReadyTurn());
    CHECK(order == std::vector<int>{2});
    CHECK_FALSE(executor.runReadyTurn());
  }

  TEST_CASE("LoopExecutor - failed turn preserves queued work behind the throwing task", "[runtime][unit][async]")
  {
    auto executor = LoopExecutor{};
    auto order = std::vector<int>{};

    executor.defer(
      [&]
      {
        order.push_back(1);
        throwException<Exception>("task failed");
      });
    executor.defer([&] { order.push_back(2); });

    CHECK_THROWS_AS(executor.runOneTurn(), Exception);
    CHECK(order == std::vector<int>{1});

    executor.defer([&] { order.push_back(3); });

    REQUIRE(executor.runReadyTurn());
    CHECK(order == std::vector<int>{1, 2});
    REQUIRE(executor.runReadyTurn());
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK_FALSE(executor.runReadyTurn());
  }
} // namespace ao::async::test
