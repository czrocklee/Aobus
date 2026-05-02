// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/IBackendProvider.h>
#include <memory>

namespace ao::audio::backend
{
  /**
   * @brief Provider for ALSA audio backends.
   */
  class AlsaProvider final : public ao::audio::IBackendProvider
  {
  public:
    AlsaProvider();
    ~AlsaProvider() override;

    ao::audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override;
    std::unique_ptr<ao::audio::IBackend> createBackend(ao::audio::Device const& device) override;
    ao::audio::Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio::backend
