// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "AlsaExclusiveBackend.h"

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <cstring>
#include <iostream>

namespace app::playback
{

  AlsaExclusiveBackend::AlsaExclusiveBackend(std::string deviceName)
    : _deviceName(std::move(deviceName))
  {
  }

  AlsaExclusiveBackend::~AlsaExclusiveBackend()
  {
    stop();
    if (_pcm)
    {
      snd_pcm_close(_pcm);
      _pcm = nullptr;
    }
  }

  void AlsaExclusiveBackend::open(StreamFormat const& format, AudioRenderCallbacks callbacks)
  {
    _format = format;
    _callbacks = callbacks;

    int err;
    snd_pcm_t* pcm = nullptr;

    // Open PCM device
    err = snd_pcm_open(&pcm, _deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
      std::cerr << "Failed to open ALSA device: " << _deviceName << std::endl;
      return;
    }

    _pcm = pcm;

    // Set hardware parameters
    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);

    err = snd_pcm_hw_params_any(_pcm, params);
    if (err < 0)
    {
      std::cerr << "Failed to initialize ALSA hardware parameters" << std::endl;
      return;
    }

    // Set interleaved access
    err = snd_pcm_hw_params_set_access(_pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0)
    {
      std::cerr << "Failed to set ALSA access mode" << std::endl;
      return;
    }

    // Set sample format (S16)
    snd_pcm_format_t formatMask = SND_PCM_FORMAT_S16;
    err = snd_pcm_hw_params_set_format(_pcm, params, formatMask);
    if (err < 0)
    {
      std::cerr << "Failed to set ALSA sample format" << std::endl;
      return;
    }

    // Set sample rate
    err = snd_pcm_hw_params_set_rate(_pcm, params, format.sampleRate, 0);
    if (err < 0)
    {
      std::cerr << "Failed to set ALSA sample rate" << std::endl;
      return;
    }

    // Set channel count
    err = snd_pcm_hw_params_set_channels(_pcm, params, format.channels);
    if (err < 0)
    {
      std::cerr << "Failed to set ALSA channel count" << std::endl;
      return;
    }

    // Apply hardware parameters
    err = snd_pcm_hw_params(_pcm, params);
    if (err < 0)
    {
      std::cerr << "Failed to apply ALSA hardware parameters" << std::endl;
      return;
    }
  }

  void AlsaExclusiveBackend::start()
  {
    if (!_pcm)
    {
      return;
    }
    snd_pcm_start(_pcm);
  }

  void AlsaExclusiveBackend::pause()
  {
    if (!_pcm)
    {
      return;
    }
    snd_pcm_pause(_pcm, 1);
  }

  void AlsaExclusiveBackend::resume()
  {
    if (!_pcm)
    {
      return;
    }
    snd_pcm_pause(_pcm, 0);
  }

  void AlsaExclusiveBackend::flush()
  {
    if (!_pcm)
    {
      return;
    }
    snd_pcm_drop(_pcm);
  }

  void AlsaExclusiveBackend::stop()
  {
    if (!_pcm)
    {
      return;
    }
    snd_pcm_drop(_pcm);
    snd_pcm_prepare(_pcm);
  }

  DeviceCapabilities AlsaExclusiveBackend::queryCapabilities() const
  {
    DeviceCapabilities caps;

    if (!_pcm)
    {
      return caps;
    }

    // Query available sample rates
    std::uint32_t rate = 0;
    for (auto const targetRate : {44100, 48000, 88200, 96000, 176400, 192000})
    {
      if (snd_pcm_hw_params_test_rate(_pcm, nullptr, targetRate, 0) == 0)
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
      if (snd_pcm_hw_params_test_format(_pcm, nullptr, fmt) == 0)
      {
        caps.bitDepths.push_back(static_cast<std::uint8_t>(depth));
      }
    }

    // Query available channel counts
    for (auto const channels : {1, 2, 4, 6, 8})
    {
      if (snd_pcm_hw_params_test_channels(_pcm, nullptr, channels) == 0)
      {
        caps.channelCounts.push_back(static_cast<std::uint8_t>(channels));
      }
    }

    return caps;
  }

} // namespace app::playback