// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>

#include <functional>
#include <vector>

namespace ao::audio::backend::detail
{
  /** @brief Deterministic monitor and shutdown hooks for CoreAudioProvider tests. */
  struct CoreAudioProviderMonitorHooks final
  {
    std::function<std::vector<Device>()> enumerateDevices;
    std::function<void()> requestRefresh;
    std::function<void()> onRefreshComplete;
    std::function<void()> onMonitorExit;
    std::function<void()> onMonitorStateDestroyed;
    std::function<void()> onShutdownStarted;
    std::function<void()> onShutdownWait;
  };
} // namespace ao::audio::backend::detail
