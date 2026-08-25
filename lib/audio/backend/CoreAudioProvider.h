// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendProvider.h>

#include <memory>
#include <string_view>

namespace ao::audio::backend::detail
{
  struct CoreAudioProviderMonitorHooks;
}

namespace ao::audio::backend
{
  /** @brief Provider for concrete macOS Core Audio output devices. */
  class CoreAudioProvider final : public BackendProvider
  {
  public:
    CoreAudioProvider();
    explicit CoreAudioProvider(std::shared_ptr<detail::CoreAudioProviderMonitorHooks> monitorHooksPtr);
    ~CoreAudioProvider() override;

    CoreAudioProvider(CoreAudioProvider const&) = delete;
    CoreAudioProvider& operator=(CoreAudioProvider const&) = delete;
    CoreAudioProvider(CoreAudioProvider&&) = delete;
    CoreAudioProvider& operator=(CoreAudioProvider&&) = delete;

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
