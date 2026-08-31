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

namespace ao::audio::backend::detail
{
  struct PipeWireMonitorHooks;
}

namespace ao::audio::backend
{
  /**
   * PipeWire graph monitor with provider-independent publication state.
   * Subscriptions remain safe after monitor destruction, and callbacks may
   * synchronously stop or destroy the monitor.
   */
  class PipeWireMonitor final
  {
  public:
    PipeWireMonitor();
    explicit PipeWireMonitor(std::shared_ptr<detail::PipeWireMonitorHooks> monitorHooksPtr);
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
    bool isRunning() const noexcept;

    Subscription subscribeGraph(std::string_view routeAnchor, std::function<void(flow::Graph const&)> callback);

  private:
    struct State;
    struct Worker;
    std::shared_ptr<State> _statePtr;
    std::unique_ptr<Worker> _workerPtr;
  };
} // namespace ao::audio::backend
