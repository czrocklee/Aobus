// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>
#include <ao/audio/flow/Graph.h>

#include <functional>
#include <string_view>
#include <vector>

namespace ao::audio::backend::detail
{
  /** @brief Deterministic publication and shutdown hooks for PipeWire monitor tests. */
  struct PipeWireMonitorHooks final
  {
    std::function<std::vector<Device>()> enumerateSinks;
    std::function<flow::Graph(std::string_view)> graphForRoute;
    std::function<void()> requestRefresh;
    std::function<void()> onGraphSubscriptionInserted;
    std::function<void()> onRefreshPrepared;
    std::function<void()> onRefreshComplete;
    std::function<void()> onMonitorExit;
    std::function<void()> onMonitorStateDestroyed;
    std::function<void()> onShutdownStarted;
    std::function<void()> onShutdownWait;
  };
} // namespace ao::audio::backend::detail
