// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/FormatNegotiator.h"
#include "core/playback/IAudioBackend.h"

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <memory>
#include <string>
#include <string_view>

namespace app::playback
{

  /**
   * @brief Backend using direct ALSA access.
   *
   * Can provide bit-perfect exclusive access when using "hw:" devices.
   */
  class AlsaExclusiveBackend final : public app::core::playback::IAudioBackend
  {
  public:
    explicit AlsaExclusiveBackend(std::string deviceName = "default");
    ~AlsaExclusiveBackend() override;

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
      return app::core::playback::BackendKind::AlsaExclusive;
    }

    std::string_view lastError() const noexcept override { return _lastError; }

    app::core::playback::DeviceCapabilities queryCapabilities() const;

  private:
    struct AlsaPcmDeleter final
    {
      void operator()(::snd_pcm_t* pcm) const noexcept { ::snd_pcm_close(pcm); }
    };
    using AlsaPcmPtr = std::unique_ptr<::snd_pcm_t, AlsaPcmDeleter>;

    std::string _deviceName;
    AlsaPcmPtr _pcm;
    app::core::playback::AudioRenderCallbacks _callbacks;
    app::core::playback::StreamFormat _format;
    std::string _lastError;
  };

} // namespace app::playback
