// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors
#pragma once

#include <rs/audio/AudioFormat.h>
#include <rs/audio/BackendTypes.h>

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

    void setDevicesChangedCallback(std::function<void()> callback);
    std::vector<rs::audio::AudioDevice> enumerateSinks() const;
    std::optional<std::uint32_t> findSinkIdByName(std::string_view name) const;

    std::uint64_t subscribeGraph(std::string_view routeAnchor,
                                 std::function<void(rs::audio::AudioGraph const&)> callback);
    void unsubscribeGraph(std::uint64_t id);

  private:
    std::unique_ptr<Impl> _impl;
  };

} // namespace app::playback
