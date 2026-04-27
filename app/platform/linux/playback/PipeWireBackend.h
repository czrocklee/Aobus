// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/IAudioBackend.h"
#include "core/backend/IDeviceDiscovery.h"

#include <memory>
#include <string_view>

namespace app::playback
{

  /**
   * @brief Audio backend using PipeWire.
   *
   * This backend provides shared playback through the PipeWire server.
   */
  class PipeWireBackend final : public app::core::backend::IAudioBackend
  {
  public:
    static std::unique_ptr<app::core::backend::IDeviceDiscovery> createDiscovery();

    struct Impl;

    explicit PipeWireBackend(app::core::backend::AudioDevice const& device);
    ~PipeWireBackend() override;

    bool open(app::core::AudioFormat const& format, app::core::backend::AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void drain() override;
    void stop() override;
    void close() override;

    void setExclusiveMode(bool exclusive) override;
    bool isExclusiveMode() const noexcept override;

    app::core::backend::BackendKind kind() const noexcept override;
    std::string_view lastError() const noexcept override;

  private:
    std::unique_ptr<Impl> _impl;
    std::string _targetDeviceId;
    bool _exclusiveMode = false;
  };

} // namespace app::playback
