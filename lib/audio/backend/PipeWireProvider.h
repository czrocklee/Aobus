// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/utility/ScopedRegistration.h>

#include <memory>
#include <string_view>

namespace ao::audio
{
  struct Device;
}

namespace ao::audio::backend::detail
{
  struct PipeWireMonitorHooks;
}

namespace ao::audio::backend
{
  /**
   * @brief Provider for PipeWire audio backends.
   */
  class PipeWireProvider final : public BackendProvider
  {
  public:
    PipeWireProvider();
    explicit PipeWireProvider(std::shared_ptr<detail::PipeWireMonitorHooks> monitorHooksPtr);
    ~PipeWireProvider() override;

    PipeWireProvider(PipeWireProvider const&) = delete;
    PipeWireProvider& operator=(PipeWireProvider const&) = delete;
    PipeWireProvider(PipeWireProvider&&) = delete;
    PipeWireProvider& operator=(PipeWireProvider&&) = delete;

    void shutdown() noexcept override;
    utility::ScopedRegistration subscribeDevices(OnDevicesChangedCallback callback) override;
    BackendProvider::Status status() const override;
    std::unique_ptr<Backend> createBackend(Device const& device, ProfileId const& profile) override;
    utility::ScopedRegistration subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio::backend
