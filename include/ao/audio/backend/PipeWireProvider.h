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
  class PipeWireProvider final : public ao::audio::IBackendProvider
  {
  public:
    PipeWireProvider();
    ~PipeWireProvider() override;

    ao::audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override;
    ao::audio::IBackendProvider::Status status() const override;
    std::unique_ptr<ao::audio::IBackend> createBackend(ao::audio::Device const& device,
                                                       ao::audio::ProfileId const& profile) override;
    ao::audio::Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio::backend
