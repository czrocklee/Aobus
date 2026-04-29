// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/IAudioBackend.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

extern "C"
{
#include <alsa/asoundlib.h>
}

namespace app::playback
{

  /**
   * @brief Audio backend using ALSA in exclusive (hardware) mode.
   */
  class AlsaExclusiveBackend final : public app::core::backend::IAudioBackend
  {
  public:
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

    void setExclusiveMode(bool exclusive) override;
    bool isExclusiveMode() const noexcept override;

    app::core::backend::BackendKind kind() const noexcept override;
    std::string_view lastError() const noexcept override;

  private:
    struct AlsaPcmDeleter
    {
      void operator()(::snd_pcm_t* p) const noexcept
      {
        if (p) ::snd_pcm_close(p);
      }
    };
    using AlsaPcmPtr = std::unique_ptr<::snd_pcm_t, AlsaPcmDeleter>;

    void playbackLoop(std::stop_token stopToken);
    void recoverFromXrun(int err);

    std::string _deviceName;
    app::core::AudioFormat _format;
    app::core::backend::AudioRenderCallbacks _callbacks;
    std::string _lastError;

    AlsaPcmPtr _pcm;
    std::jthread _thread;
    std::atomic<bool> _paused{false};
  };

} // namespace app::playback
