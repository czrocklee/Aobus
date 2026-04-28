// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/IAudioBackend.h"
#include "core/backend/IBackendManager.h"

#include <memory>
#include <vector>

namespace app::core::backend
{

  /**
   * @brief A backend that does nothing. Used as a fallback or for testing.
   */
  class NullBackend final : public IAudioBackend
  {
  public:
    class NullManager final : public IBackendManager
    {
    public:
      void setDevicesChangedCallback(OnDevicesChangedCallback /*callback*/) override {}
      std::vector<AudioDevice> enumerateDevices() override
      {
        return {{.id = "null",
                 .displayName = "None",
                 .description = "No audio output",
                 .backendKind = BackendKind::None,
                 .capabilities = {}}};
      }

      std::unique_ptr<IAudioBackend> createBackend(AudioDevice const& /*device*/) override
      {
        return std::make_unique<NullBackend>();
      }
    };

    NullBackend() = default;
    ~NullBackend() override = default;

    bool open(AudioFormat const& /*format*/, AudioRenderCallbacks callbacks) override
    {
      if (callbacks.onGraphChanged)
      {
        callbacks.onGraphChanged(callbacks.userData, {});
      }
      return true;
    }

    void start() override {}
    void pause() override {}
    void resume() override {}
    void flush() override {}
    void drain() override
    {
      // No-op, but we should probably signal completion if requested
    }
    void stop() override {}
    void close() override {}

    void setExclusiveMode(bool /*exclusive*/) override {}
    bool isExclusiveMode() const noexcept override { return false; }

    BackendKind kind() const noexcept override { return BackendKind::None; }
    std::string_view lastError() const noexcept override { return ""; }
  };

} // namespace app::core::backend
