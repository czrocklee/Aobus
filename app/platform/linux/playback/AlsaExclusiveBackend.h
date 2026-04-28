// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/IAudioBackend.h"
#include "core/backend/IDeviceDiscovery.h"
#include "core/playback/FormatNegotiator.h"

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace app::playback
{

  /**
   * @brief Backend using direct ALSA access.
   *
   * Can provide bit-perfect exclusive access when using "hw:" devices.
   */
  class AlsaExclusiveBackend final : public app::core::backend::IAudioBackend
  {
  public:
    static std::unique_ptr<app::core::backend::IDeviceDiscovery> createDiscovery();

    explicit AlsaExclusiveBackend(app::core::backend::AudioDevice const& device);
    ~AlsaExclusiveBackend() override;

    bool open(app::core::AudioFormat const& format, app::core::backend::AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void drain() override;
    void stop() override;
    void close() override;

    void setExclusiveMode(bool) override {}
    bool isExclusiveMode() const noexcept override { return true; } // ALSA is always exclusive

    app::core::backend::BackendKind kind() const noexcept override
    {
      return app::core::backend::BackendKind::AlsaExclusive;
    }

    std::string_view lastError() const noexcept override { return _lastError; }

  private:
    struct AlsaPcmDeleter final
    {
      void operator()(::snd_pcm_t* pcm) const noexcept { ::snd_pcm_close(pcm); }
    };
    using AlsaPcmPtr = std::unique_ptr<::snd_pcm_t, AlsaPcmDeleter>;

    void playbackLoop(std::stop_token stopToken);
    void recoverFromXrun(int err);

    std::string _deviceName;
    AlsaPcmPtr _pcm;
    app::core::backend::AudioRenderCallbacks _callbacks;
    app::core::AudioFormat _format;
    std::string _lastError;

    std::stop_token _stopToken;
    std::jthread _thread;
    std::atomic<bool> _paused{false};
  };

} // namespace app::playback
