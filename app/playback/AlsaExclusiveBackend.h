// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "FormatNegotiator.h"
#include "IAudioBackend.h"

extern "C"
{
#include <alsa/pcm.h>
}

#include <string>
#include <string_view>

namespace app::playback
{

  class AlsaExclusiveBackend final : public IAudioBackend
  {
  public:
    explicit AlsaExclusiveBackend(std::string deviceName = "default");
    ~AlsaExclusiveBackend() override;

    bool open(StreamFormat const& format, AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void drain() override;
    void stop() override;
    void close() override;
    BackendKind kind() const noexcept override { return BackendKind::AlsaExclusive; }
    BackendFormatInfo formatInfo() const override { return _formatInfo; }
    std::string_view lastError() const noexcept override { return _lastError; }

    DeviceCapabilities queryCapabilities() const;

  private:
    std::string _deviceName;
    snd_pcm_t* _pcm = nullptr;
    AudioRenderCallbacks _callbacks;
    StreamFormat _format;
    BackendFormatInfo _formatInfo;
    std::string _lastError;
  };

} // namespace app::playback
