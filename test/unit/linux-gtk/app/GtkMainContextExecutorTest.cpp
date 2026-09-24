// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkMainContextExecutor.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("GtkMainContextExecutor - dispatch runs immediately on the owner thread", "[gtk][unit][app][executor]")
  {
    auto executor = GtkMainContextExecutor{};
    bool ran = false;

    executor.dispatch(
      [&]
      {
        ran = true;
        CHECK(executor.isCurrent());
      });

    CHECK(ran);
  }

  TEST_CASE("GtkMainContextExecutor - foreign dispatch and owner defer wait for a main-context turn",
            "[gtk][unit][app][executor][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = GtkMainContextExecutor{};
    auto executionOrder = std::vector<std::string>{};
    auto ranOnOwner = std::vector<bool>{};
    auto callbackCount = std::atomic_size_t{0};
    auto producerReturned = std::latch{1};

    auto producer = std::jthread{[&]
                                 {
                                   executor.dispatch(
                                     [&]
                                     {
                                       executionOrder.emplace_back("foreign dispatch");
                                       ranOnOwner.push_back(executor.isCurrent());
                                       callbackCount.fetch_add(1, std::memory_order_release);
                                     });
                                   producerReturned.count_down();
                                 }};
    producerReturned.wait();
    producer.join();

    executor.defer(
      [&]
      {
        executionOrder.emplace_back("owner defer");
        ranOnOwner.push_back(executor.isCurrent());
        callbackCount.fetch_add(1, std::memory_order_release);
      });

    CHECK(callbackCount.load(std::memory_order_acquire) == 0);
    REQUIRE(tryPumpGtkEventsUntil([&] { return callbackCount.load(std::memory_order_acquire) == 2; }));

    auto const expectedOrder = std::vector<std::string>{"foreign dispatch", "owner defer"};
    auto const expectedAffinity = std::vector{true, true};
    CHECK(executionOrder == expectedOrder);
    CHECK(ranOnOwner == expectedAffinity);
    CHECK(callbackCount.load(std::memory_order_acquire) == 2);
  }
} // namespace ao::gtk::test
