// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors
#pragma once

#include "core/AudioFormat.h"
#include "core/backend/BackendTypes.h"

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
    std::vector<app::core::backend::AudioDevice> enumerateSinks() const;
    std::optional<std::uint32_t> findSinkIdByName(std::string_view name) const;

    std::uint64_t subscribeGraph(std::string_view routeAnchor,
                                 std::function<void(app::core::backend::AudioGraph const&)> callback);
    void unsubscribeGraph(std::uint64_t id);

  private:
    std::unique_ptr<Impl> _impl;
  };

} // namespace app::playback
