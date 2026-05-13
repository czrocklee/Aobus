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
  class AlsaProvider final : public IBackendProvider
  {
  public:
    AlsaProvider();
    ~AlsaProvider() override;

    Subscription subscribeDevices(OnDevicesChangedCallback callback) override;
    IBackendProvider::Status status() const override;
    std::unique_ptr<IBackend> createBackend(Device const& device, ProfileId const& profile) override;
    Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio::backend
