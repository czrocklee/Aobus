// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>

#include <memory>
#include <string_view>

namespace ao::audio::backend
{
  /**
   * @brief Provider for ALSA audio backends.
   */
  class AlsaProvider final : public BackendProvider
  {
  public:
    AlsaProvider();
    ~AlsaProvider() override;

    AlsaProvider(AlsaProvider const&) = delete;
    AlsaProvider& operator=(AlsaProvider const&) = delete;
    AlsaProvider(AlsaProvider&&) = delete;
    AlsaProvider& operator=(AlsaProvider&&) = delete;

    void shutdown() noexcept override;
    Subscription subscribeDevices(OnDevicesChangedCallback callback) override;
    BackendProvider::Status status() const override;
    std::unique_ptr<Backend> createBackend(Device const& device, ProfileId const& profile) override;
    Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio::backend
