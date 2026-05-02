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
    struct Impl;
    PipeWireMonitor();
    ~PipeWireMonitor();

    void start();
    void stop();
    void refresh();

    using DeviceCallback = std::function<void(std::vector<ao::audio::Device> const&)>;
    ao::audio::Subscription subscribeDevices(DeviceCallback callback);

    std::vector<ao::audio::Device> enumerateSinks() const;
    std::optional<std::uint32_t> findSinkIdByName(std::string_view name) const;

    ao::audio::Subscription subscribeGraph(std::string_view routeAnchor,
                                           std::function<void(ao::audio::flow::Graph const&)> callback);

  private:
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio::backend
