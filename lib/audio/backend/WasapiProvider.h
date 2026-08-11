// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>

#include <memory>
#include <string_view>

namespace ao::audio::backend::detail
{
  struct WasapiProviderMonitorHooks;
}

namespace ao::audio::backend
{
  /**
   * @brief Provider for WASAPI audio backends (Windows).
   */
  class WasapiProvider final : public BackendProvider
  {
  public:
    WasapiProvider();
    explicit WasapiProvider(std::shared_ptr<detail::WasapiProviderMonitorHooks> monitorHooksPtr);
    ~WasapiProvider() override;

    WasapiProvider(WasapiProvider const&) = delete;
    WasapiProvider& operator=(WasapiProvider const&) = delete;
    WasapiProvider(WasapiProvider&&) = delete;
    WasapiProvider& operator=(WasapiProvider&&) = delete;

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
