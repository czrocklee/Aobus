// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "IAudioBackend.h"

namespace app::playback
{

  // Fallback backend that does nothing - used when no real backend is available
  class NullBackend final : public IAudioBackend
  {
  public:
    NullBackend() = default;
    ~NullBackend() override = default;

    void open(StreamFormat const& /*format*/, AudioRenderCallbacks /*callbacks*/) override {}
    void start() override {}
    void pause() override {}
    void resume() override {}
    void flush() override {}
    void stop() override {}
    BackendKind kind() const noexcept override { return BackendKind::None; }
  };

} // namespace app::playback