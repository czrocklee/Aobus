// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Device.h>
#include <ao/audio/backend/PipeWireMonitor.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

namespace ao::audio::backend::test
{
  // NOTE: The `stopping` atomic in PipeWireMonitor::Impl guards a narrow race where refresh()
  // has already captured subscription callbacks into local vectors (under lock) and stop() fires
  // concurrently on another thread. This race cannot be reproduced deterministically in a
  // single-threaded test. The tests below verify the observable single-threaded contract:
  // after stop(), no further callbacks are invoked.
  TEST_CASE("PipeWireMonitor - stop prevents later refresh callbacks", "[audio][unit][pipewire][monitor]")
  {
    auto monitor = PipeWireMonitor{};
    std::int32_t deviceCallbackCount = 0;
    std::int32_t graphCallbackCount = 0;

    auto deviceSub = monitor.subscribeDevices([&](std::vector<Device> const&) { deviceCallbackCount++; });
    auto graphSub = monitor.subscribeGraph("42", [&](flow::Graph const&) { graphCallbackCount++; });

    monitor.stop();
    auto const deviceCallbacksAfterStop = deviceCallbackCount;
    auto const graphCallbacksAfterStop = graphCallbackCount;

    monitor.refresh();

    CHECK(deviceCallbackCount == deviceCallbacksAfterStop);
    CHECK(graphCallbackCount == graphCallbacksAfterStop);
  }

  TEST_CASE("PipeWireMonitor - stop is idempotent", "[audio][unit][pipewire][monitor]")
  {
    auto monitor = PipeWireMonitor{};
    auto deviceSub = monitor.subscribeDevices([](std::vector<Device> const&) {});

    monitor.stop();
    monitor.stop(); // Must not crash or deadlock.
  }

  TEST_CASE("PipeWireMonitor - subscription handles are safe to destroy after stop", "[audio][unit][pipewire][monitor]")
  {
    auto monitor = PipeWireMonitor{};

    auto deviceSub = monitor.subscribeDevices([](std::vector<Device> const&) {});
    auto graphSub = monitor.subscribeGraph("42", [](flow::Graph const&) {});

    monitor.stop(); // Clears internal subscription lists.

    // Destroying the subscription handles triggers their unsubscribe lambdas, which try to
    // erase from the (now-empty) internal lists. This must not crash.
    deviceSub.reset();
    graphSub.reset();
  }

  TEST_CASE("PipeWireMonitor - graph callback may destroy the monitor off the native loop",
            "[audio][regression][pipewire][monitor]")
  {
    auto monitorPtr = std::make_unique<PipeWireMonitor>();
    auto callbackStarted = std::binary_semaphore{0};
    auto allowDestroy = std::binary_semaphore{0};
    auto monitorDestroyed = std::binary_semaphore{0};
    auto callbackThreadId = std::thread::id{};
    auto const subscribeThreadId = std::this_thread::get_id();

    auto graphSub = monitorPtr->subscribeGraph("42",
                                               [&](flow::Graph const&)
                                               {
                                                 callbackThreadId = std::this_thread::get_id();
                                                 callbackStarted.release();
                                                 allowDestroy.acquire();
                                                 monitorPtr.reset();
                                                 monitorDestroyed.release();
                                               });

    REQUIRE(callbackStarted.try_acquire_for(std::chrono::seconds{5}));
    allowDestroy.release();
    REQUIRE(monitorDestroyed.try_acquire_for(std::chrono::seconds{5}));
    CHECK_FALSE(monitorPtr);
    CHECK(callbackThreadId != subscribeThreadId);

    // The monitor is already gone; the handle must use weak state and remain safe.
    graphSub.reset();
  }

  TEST_CASE("PipeWireMonitor - cancellation suppresses a callback copied by refresh",
            "[audio][regression][pipewire][monitor]")
  {
    auto monitor = PipeWireMonitor{};
    auto firstDelivered = std::binary_semaphore{0};
    auto secondDelivered = std::binary_semaphore{0};
    auto cancelSecond = std::atomic{false};
    auto firstCallbackCount = std::atomic<std::int32_t>{0};
    auto secondCallbackCount = std::atomic<std::int32_t>{0};
    auto secondSub = Subscription{};

    auto firstSub = monitor.subscribeGraph("42",
                                           [&](flow::Graph const&)
                                           {
                                             if (firstCallbackCount.fetch_add(1, std::memory_order_relaxed) == 0)
                                             {
                                               firstDelivered.release();
                                             }

                                             if (cancelSecond.load(std::memory_order_acquire))
                                             {
                                               secondSub.reset();
                                             }
                                           });
    REQUIRE(firstDelivered.try_acquire_for(std::chrono::seconds{5}));

    secondSub = monitor.subscribeGraph("43",
                                       [&](flow::Graph const&)
                                       {
                                         if (secondCallbackCount.fetch_add(1, std::memory_order_relaxed) == 0)
                                         {
                                           secondDelivered.release();
                                         }
                                       });
    REQUIRE(secondDelivered.try_acquire_for(std::chrono::seconds{5}));

    auto const secondCallbacksBeforeCancellation = secondCallbackCount.load(std::memory_order_relaxed);
    cancelSecond.store(true, std::memory_order_release);
    monitor.refresh();

    CHECK(secondCallbackCount.load(std::memory_order_relaxed) == secondCallbacksBeforeCancellation);
  }
} // namespace ao::audio::backend::test
