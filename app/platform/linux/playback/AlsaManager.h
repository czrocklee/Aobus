// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <memory>
#include <rs/audio/IBackendManager.h>

namespace app::playback
{
  /**
   * @brief Manager for ALSA audio backends.
   */
  class AlsaManager final : public rs::audio::IBackendManager
  {
  public:
    AlsaManager();
    ~AlsaManager() override;

    void setDevicesChangedCallback(OnDevicesChangedCallback callback) override;
    std::vector<rs::audio::AudioDevice> enumerateDevices() override;
    std::unique_ptr<rs::audio::IBackend> createBackend(rs::audio::AudioDevice const& device) override;
    std::unique_ptr<rs::audio::IGraphSubscription> subscribeGraph(std::string_view routeAnchor,
                                                                  OnGraphChangedCallback callback) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace app::playback
