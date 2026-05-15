// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/IBackendProvider.h>
#include <memory>

namespace ao::audio::backend
{
  /**
   * @brief Provider for PipeWire audio backends.
   */
  class PipeWireProvider final : public IBackendProvider
  {
  public:
    PipeWireProvider();
    ~PipeWireProvider() override;

    PipeWireProvider(PipeWireProvider const&) = delete;
    PipeWireProvider& operator=(PipeWireProvider const&) = delete;
    PipeWireProvider(PipeWireProvider&&) = delete;
    PipeWireProvider& operator=(PipeWireProvider&&) = delete;

    Subscription subscribeDevices(OnDevicesChangedCallback callback) override;
    IBackendProvider::Status status() const override;
    std::unique_ptr<IBackend> createBackend(Device const& device, ProfileId const& profile) override;
    Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio::backend
