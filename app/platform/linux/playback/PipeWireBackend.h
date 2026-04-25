// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/IAudioBackend.h"

#include <memory>
#include <string_view>

namespace app::playback
{

  /**
   * @brief Audio backend using PipeWire.
   *
   * This backend provides shared playback through the PipeWire server.
   */
  class PipeWireBackend final : public app::core::playback::IAudioBackend
  {
  public:
    struct Impl;

    PipeWireBackend();
    ~PipeWireBackend() override;

    bool open(app::core::playback::StreamFormat const& format,
              app::core::playback::AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void drain() override;
    void stop() override;
    void close() override;

    app::core::playback::BackendKind kind() const noexcept override
    {
      return app::core::playback::BackendKind::PipeWire;
    }

    std::string_view lastError() const noexcept override;

  private:
    std::unique_ptr<Impl> _impl;
  };

} // namespace app::playback
