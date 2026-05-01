// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <memory>
#include <rs/audio/IBackendProvider.h>

namespace app::playback
{
  /**
   * @brief Provider for PipeWire audio backends.
   */
  class PipeWireProvider final : public rs::audio::IBackendProvider
  {
  public:
    PipeWireProvider();
    ~PipeWireProvider() override;

    rs::audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override;
    std::unique_ptr<rs::audio::IBackend> createBackend(rs::audio::Device const& device) override;
    rs::audio::Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace app::playback
