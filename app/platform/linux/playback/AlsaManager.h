// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/IBackendManager.h"
#include <memory>

namespace app::playback
{
  /**
   * @brief Manager for ALSA audio backends.
   */
  class AlsaManager final : public app::core::backend::IBackendManager
  {
  public:
    AlsaManager();
    ~AlsaManager() override;

    void setDevicesChangedCallback(OnDevicesChangedCallback callback) override;
    std::vector<app::core::backend::AudioDevice> enumerateDevices() override;
    std::unique_ptr<app::core::backend::IAudioBackend> createBackend(app::core::backend::AudioDevice const& device) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace app::playback
