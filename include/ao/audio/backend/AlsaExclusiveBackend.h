// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/IBackend.h>

#include <memory>

namespace ao::audio::backend
{
  /**
   * @brief Audio backend using ALSA in exclusive (hardware) mode.
   */
  class AlsaExclusiveBackend final : public ao::audio::IBackend
  {
  public:
    explicit AlsaExclusiveBackend(ao::audio::Device const& device, ao::audio::ProfileId const& profile);
    ~AlsaExclusiveBackend() override;

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
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio::backend
