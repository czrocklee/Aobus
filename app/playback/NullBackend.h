// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "IAudioBackend.h"

#include <string_view>

namespace app::playback
{

  // Fallback backend that does nothing - used when no real backend is available
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
    BackendFormatInfo formatInfo() const override { return {}; }
    std::string_view lastError() const noexcept override { return {}; }

  private:
    AudioRenderCallbacks _callbacks{};
  };

} // namespace app::playback
