// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/backend/PipeWireMonitor.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
} // namespace ao::audio::backend::test
