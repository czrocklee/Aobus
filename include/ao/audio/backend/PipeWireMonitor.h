// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors
#pragma once

#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace ao::audio::backend
{
  class PipeWireMonitor final
  {
  public:
    PipeWireMonitor();
    ~PipeWireMonitor();

    PipeWireMonitor(PipeWireMonitor const&) = delete;
    PipeWireMonitor& operator=(PipeWireMonitor const&) = delete;
    PipeWireMonitor(PipeWireMonitor&&) = delete;
    PipeWireMonitor& operator=(PipeWireMonitor&&) = delete;

    void start();
    void stop();
    void refresh();

    using DeviceCallback = std::function<void(std::vector<Device> const&)>;
    Subscription subscribeDevices(DeviceCallback callback);

    std::vector<Device> enumerateSinks() const;

    Subscription subscribeGraph(std::string_view routeAnchor, std::function<void(flow::Graph const&)> callback);

  private:
    struct Impl;
    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::audio::backend
