// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "FormatNegotiator.h"
#include "IAudioBackend.h"

extern "C"
{
#include <alsa/pcm.h>
}

namespace app::playback
{

  class AlsaExclusiveBackend final : public IAudioBackend
  {
  public:
    explicit AlsaExclusiveBackend(std::string deviceName = "default");
    ~AlsaExclusiveBackend() override;

    void open(StreamFormat const& format, AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    BackendKind kind() const noexcept override { return BackendKind::AlsaExclusive; }

    DeviceCapabilities queryCapabilities() const;

  private:
    std::string _deviceName;
    snd_pcm_t* _pcm = nullptr;
    AudioRenderCallbacks _callbacks;
    StreamFormat _format;
  };

} // namespace app::playback