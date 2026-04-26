// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/IAudioBackend.h"
#include "core/playback/IDeviceDiscovery.h"

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
    class NullDiscovery final : public IDeviceDiscovery
    {
    public:
      void setDevicesChangedCallback(OnDevicesChangedCallback) override {}
      std::vector<AudioDevice> enumerateDevices() override { return {}; }
      std::unique_ptr<IAudioBackend> createBackend(AudioDevice const&) override
      {
        return std::make_unique<NullBackend>();
      }
    };

    static std::unique_ptr<IDeviceDiscovery> createDiscovery() { return std::make_unique<NullDiscovery>(); }

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

    void setExclusiveMode(bool) override {}
    bool isExclusiveMode() const noexcept override { return false; }

    BackendKind kind() const noexcept override { return BackendKind::None; }
    std::string_view lastError() const noexcept override { return {}; }

  private:
    AudioRenderCallbacks _callbacks{};
  };

} // namespace app::core::playback
