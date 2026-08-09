// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/ExecutorTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void checkRejection(std::exception_ptr const& failure, std::string_view const expectedMessage)
    {
      REQUIRE(failure);

      try
      {
        std::rethrow_exception(failure);
      }
      catch (std::runtime_error const& exception)
      {
        CHECK(std::string_view{exception.what()} == expectedMessage);
      }
      catch (...)
      {
        FAIL("Executor rejected work with an unexpected exception type");
      }
    }
  } // namespace

  TEST_CASE("InlineExecutor - owner work executes inline", "[runtime][unit][async]")
  {
    auto executor = InlineExecutor{};
    auto order = std::vector<int>{};

    CHECK(executor.isCurrent());

    executor.dispatch([&] { order.push_back(1); });
    executor.defer([&] { order.push_back(2); });
    executor.dispatch({});
    executor.defer({});

    CHECK(order == std::vector<int>{1, 2});
  }

  TEST_CASE("InlineExecutor - foreign work is rejected", "[runtime][regression][async][concurrency]")
  {
    auto executor = InlineExecutor{};
    bool taskRan = false;
    bool workerWasCurrent = true;
    auto dispatchFailure = std::exception_ptr{};
    auto deferFailure = std::exception_ptr{};

    auto worker = std::jthread{[&]
                               {
                                 workerWasCurrent = executor.isCurrent();

                                 try
                                 {
                                   executor.dispatch([&] { taskRan = true; });
                                 }
                                 catch (...)
                                 {
                                   dispatchFailure = std::current_exception();
                                 }

                                 try
                                 {
                                   executor.defer([&] { taskRan = true; });
                                 }
                                 catch (...)
                                 {
                                   deferFailure = std::current_exception();
                                 }

                                 executor.dispatch({});
                                 executor.defer({});
                               }};
    worker.join();

    CHECK_FALSE(workerWasCurrent);
    CHECK_FALSE(taskRan);
    checkRejection(dispatchFailure, "InlineExecutor can only execute work on its owner thread");
    checkRejection(deferFailure, "InlineExecutor can only execute work on its owner thread");
  }

  TEST_CASE("ManualExecutor - foreign producers queue work for the owner", "[runtime][unit][async][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto const ownerThread = std::this_thread::get_id();
    auto callbackThread = std::thread::id{};
    bool workerWasCurrent = true;
    auto drainFailure = std::exception_ptr{};

    auto worker = std::jthread{[&]
                               {
                                 workerWasCurrent = executor.isCurrent();
                                 executor.dispatch([&] { callbackThread = std::this_thread::get_id(); });

                                 try
                                 {
                                   std::ignore = executor.runOne();
                                 }
                                 catch (...)
                                 {
                                   drainFailure = std::current_exception();
                                 }
                               }};
    worker.join();

    CHECK_FALSE(workerWasCurrent);
    CHECK(callbackThread == std::thread::id{});
    CHECK(executor.queuedCount() == 1);
    checkRejection(drainFailure, "ManualExecutor can only be drained on its owner thread");

    REQUIRE(executor.runOne());
    CHECK(callbackThread == ownerThread);
    CHECK_FALSE(executor.runOne());
  }
} // namespace ao::rt::test
