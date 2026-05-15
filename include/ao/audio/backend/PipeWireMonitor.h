// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors
#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/IBackendProvider.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
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
    std::optional<std::uint32_t> findSinkIdByName(std::string_view name) const;

    Subscription subscribeGraph(std::string_view routeAnchor, std::function<void(flow::Graph const&)> callback);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio::backend
