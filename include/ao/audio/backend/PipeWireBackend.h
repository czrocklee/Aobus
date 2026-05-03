// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/IBackend.h>
#include <memory>
#include <string_view>

namespace ao::audio::backend
{
  /**
   * @brief Audio backend using PipeWire.
   */
  class PipeWireBackend final : public ao::audio::IBackend
  {
  public:
    struct Impl;
    explicit PipeWireBackend(ao::audio::Device const& device, ao::audio::ProfileId const& profile);
    ~PipeWireBackend() override;

    ao::Result<> open(ao::audio::Format const& format, ao::audio::RenderCallbacks callbacks) override;
    void reset() override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void drain() override;
    void stop() override;
    void close() override;

    void setExclusiveMode(bool exclusive) override;
    bool isExclusiveMode() const noexcept override;

    ao::audio::BackendId backendId() const noexcept override;
    ao::audio::ProfileId profileId() const noexcept override;

  private:
    std::unique_ptr<Impl> _impl;
    std::string _targetDeviceId;
    bool _exclusiveMode = false;
  };
} // namespace ao::audio::backend
