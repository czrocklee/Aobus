// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/AlsaExclusiveBackend.h"

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <cstring>
#include <iostream>

namespace app::playback
{
  using namespace app::core::playback;

  AlsaExclusiveBackend::AlsaExclusiveBackend(std::string deviceName)
    : _deviceName(std::move(deviceName))
  {
  }

  AlsaExclusiveBackend::~AlsaExclusiveBackend()
  {
    stop();
    close();
  }

  bool AlsaExclusiveBackend::open(StreamFormat const& format, AudioRenderCallbacks callbacks)
  {
    close();

    _format = format;
    _callbacks = callbacks;
    _formatInfo = {};
    _lastError.clear();

    std::int32_t err;
    snd_pcm_t* pcm = nullptr;

    // Open PCM device
    err = ::snd_pcm_open(&pcm, _deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
      _lastError = "Failed to open ALSA device: " + _deviceName;
      return false;
    }

    _pcm = pcm;

    // Set hardware parameters
    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);

    err = ::snd_pcm_hw_params_any(_pcm, params);
    if (err < 0)
    {
      _lastError = "Failed to initialize ALSA hardware parameters";
      close();
      return false;
    }

    // Set interleaved access
    err = ::snd_pcm_hw_params_set_access(_pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0)
    {
      _lastError = "Failed to set ALSA access mode";
      close();
      return false;
    }

    // Set sample format (S16)
    snd_pcm_format_t formatMask = SND_PCM_FORMAT_S16;
    err = ::snd_pcm_hw_params_set_format(_pcm, params, formatMask);
    if (err < 0)
    {
      _lastError = "Failed to set ALSA sample format";
      close();
      return false;
    }

    // Set sample rate
    err = ::snd_pcm_hw_params_set_rate(_pcm, params, format.sampleRate, 0);
    if (err < 0)
    {
      _lastError = "Failed to set ALSA sample rate";
      close();
      return false;
    }

    // Set channel count
    err = ::snd_pcm_hw_params_set_channels(_pcm, params, format.channels);
    if (err < 0)
    {
      _lastError = "Failed to set ALSA channel count";
      close();
      return false;
    }

    // Apply hardware parameters
    err = ::snd_pcm_hw_params(_pcm, params);
    if (err < 0)
    {
      _lastError = "Failed to apply ALSA hardware parameters";
      close();
      return false;
    }

    _formatInfo.streamFormat = format;
    _formatInfo.deviceFormat = format;
    _formatInfo.isExclusive = _deviceName.rfind("hw:", 0) == 0;
    _formatInfo.sinkName = _deviceName;
    _formatInfo.sinkStatus =
      _formatInfo.isExclusive ? BackendFormatInfo::SinkStatus::Good : BackendFormatInfo::SinkStatus::Warning;
    _formatInfo.sinkTooltip =
      _formatInfo.isExclusive
        ? "Direct ALSA hw device selected. This is outside PipeWire and is the strongest available path in the app "
          "today."
        : "ALSA playback is using a non-hw device name, so bit-perfect playback is not guaranteed.";
    if (!_formatInfo.isExclusive)
    {
      _formatInfo.conversionReason = "ALSA output is using a non-hw device, so bit-perfect playback is not guaranteed";
    }

    return true;
  }

  void AlsaExclusiveBackend::start()
  {
    if (!_pcm)
    {
      return;
    }
    ::snd_pcm_start(_pcm);
  }

  void AlsaExclusiveBackend::pause()
  {
    if (!_pcm)
    {
      return;
    }
    ::snd_pcm_pause(_pcm, 1);
  }

  void AlsaExclusiveBackend::resume()
  {
    if (!_pcm)
    {
      return;
    }
    ::snd_pcm_pause(_pcm, 0);
  }

  void AlsaExclusiveBackend::flush()
  {
    if (!_pcm)
    {
      return;
    }
    ::snd_pcm_drop(_pcm);
  }

  void AlsaExclusiveBackend::drain()
  {
    if (!_pcm)
    {
      if (_callbacks.onDrainComplete)
      {
        _callbacks.onDrainComplete(_callbacks.userData);
      }
      return;
    }

    ::snd_pcm_drain(_pcm);
    if (_callbacks.onDrainComplete)
    {
      _callbacks.onDrainComplete(_callbacks.userData);
    }
  }

  void AlsaExclusiveBackend::stop()
  {
    if (!_pcm)
    {
      return;
    }
    ::snd_pcm_drop(_pcm);
    ::snd_pcm_prepare(_pcm);
  }

  void AlsaExclusiveBackend::close()
  {
    _formatInfo = {};

    if (_pcm)
    {
      ::snd_pcm_close(_pcm);
      _pcm = nullptr;
    }
  }

  DeviceCapabilities AlsaExclusiveBackend::queryCapabilities() const
  {
    DeviceCapabilities caps;

    if (!_pcm)
    {
      return caps;
    }

    // Query available sample rates
    for (auto const targetRate : {44100, 48000, 88200, 96000, 176400, 192000})
    {
      if (::snd_pcm_hw_params_test_rate(_pcm, nullptr, targetRate, 0) == 0)
      {
        caps.sampleRates.push_back(targetRate);
      }
    }

    // Query available bit depths
    for (auto const depth : {16, 24, 32})
    {
      snd_pcm_format_t fmt = (depth == 16)   ? SND_PCM_FORMAT_S16
                             : (depth == 24) ? SND_PCM_FORMAT_S24
                                             : SND_PCM_FORMAT_S32;
      if (::snd_pcm_hw_params_test_format(_pcm, nullptr, fmt) == 0)
      {
        caps.bitDepths.push_back(static_cast<std::uint8_t>(depth));
      }
    }

    // Query available channel counts
    for (auto const channels : {1, 2, 4, 6, 8})
    {
      if (::snd_pcm_hw_params_test_channels(_pcm, nullptr, channels) == 0)
      {
        caps.channelCounts.push_back(static_cast<std::uint8_t>(channels));
      }
    }

    return caps;
  }

} // namespace app::playback
