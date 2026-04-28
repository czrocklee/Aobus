// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/IBackendManager.h"
#include <memory>

namespace app::playback
{
  /**
   * @brief Manager for PipeWire audio backends.
   */
  class PipeWireManager final : public app::core::backend::IBackendManager
  {
  public:
    PipeWireManager();
    ~PipeWireManager() override;

    void setDevicesChangedCallback(OnDevicesChangedCallback callback) override;
    std::vector<app::core::backend::AudioDevice> enumerateDevices() override;
    std::unique_ptr<app::core::backend::IAudioBackend> createBackend(app::core::backend::AudioDevice const& device) override;
    std::unique_ptr<app::core::backend::IGraphSubscription> subscribeGraph(std::string_view routeAnchor,
                                                                           OnGraphChangedCallback callback) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace app::playback
