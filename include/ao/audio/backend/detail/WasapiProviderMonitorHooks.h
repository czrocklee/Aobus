// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>

#include <functional>
#include <vector>

namespace ao::audio::backend::detail
{
  /**
   * @brief Optional monitor event-source hooks for deterministic embeddings.
   *
   * When enumerateDevices is supplied, the provider does not register an
   * IMMNotificationClient. requestRefresh is populated by the provider after
   * construction and signals the same monitor event used by native endpoint
   * notifications. The lifecycle callbacks observe monitor completion without
   * depending on endpoint hardware or timing.
   */
  struct WasapiProviderMonitorHooks final
  {
    std::function<std::vector<Device>()> enumerateDevices;
    std::function<void()> requestRefresh;
    std::function<void()> onDeviceCallbacksReady;
    std::function<void()> onRefreshComplete;
    std::function<void()> onMonitorExit;
    std::function<void()> onMonitorStateDestroyed;
  };
} // namespace ao::audio::backend::detail
