// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors
#pragma once

#include <rs/audio/Backend.h>
#include <rs/audio/Format.h>
#include <rs/audio/IBackendProvider.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app::playback
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

    using DeviceCallback = std::function<void(std::vector<rs::audio::Device> const&)>;
    rs::audio::Subscription subscribeDevices(DeviceCallback callback);

    std::vector<rs::audio::Device> enumerateSinks() const;
    std::optional<std::uint32_t> findSinkIdByName(std::string_view name) const;

    rs::audio::Subscription subscribeGraph(std::string_view routeAnchor,
                                           std::function<void(rs::audio::flow::Graph const&)> callback);

  private:
    std::unique_ptr<Impl> _impl;
  };
} // namespace app::playback
