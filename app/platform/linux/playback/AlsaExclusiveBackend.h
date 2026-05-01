// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/IBackend.h>

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
  class AlsaExclusiveBackend final : public rs::audio::IBackend
  {
  public:
    explicit AlsaExclusiveBackend(rs::audio::Device const& device);
    ~AlsaExclusiveBackend() override;

    rs::Result<> open(rs::audio::Format const& format, rs::audio::RenderCallbacks callbacks) override;
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

    rs::audio::BackendKind kind() const noexcept override;

  private:
    struct AlsaPcmDeleter
    {
      void operator()(::snd_pcm_t* p) const noexcept
      {
        if (p) ::snd_pcm_close(p);
      }
    };
    using AlsaPcmPtr = std::unique_ptr<::snd_pcm_t, AlsaPcmDeleter>;

    void playbackLoop(std::stop_token const& stopToken);
    void recoverFromXrun(int err);

    std::string _deviceName;
    rs::audio::Format _format;
    rs::audio::RenderCallbacks _callbacks;

    AlsaPcmPtr _pcm;
    std::jthread _thread;
    std::atomic<bool> _paused{false};
  };
} // namespace app::playback
