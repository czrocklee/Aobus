// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/IAudioBackend.h"

#include <string_view>

namespace app::core::playback
{

  /**
   * @brief Fallback backend that does nothing.
   *
   * Used when no real backend is available.
   */
  class NullBackend final : public IAudioBackend
  {
  public:
    NullBackend() = default;
    ~NullBackend() override = default;

    bool open(StreamFormat const& /*format*/, AudioRenderCallbacks callbacks) override
    {
      _callbacks = callbacks;
      return true;
    }

    void start() override {}
    void pause() override {}
    void resume() override {}
    void flush() override {}

    void drain() override
    {
      if (_callbacks.onDrainComplete)
      {
        _callbacks.onDrainComplete(_callbacks.userData);
      }
    }

    void stop() override {}
    void close() override {}

    BackendKind kind() const noexcept override { return BackendKind::None; }
    std::string_view lastError() const noexcept override { return {}; }

  private:
    AudioRenderCallbacks _callbacks{};
  };

} // namespace app::core::playback
