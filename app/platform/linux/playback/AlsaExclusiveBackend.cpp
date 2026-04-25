// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/AlsaExclusiveBackend.h"
#include "core/Log.h"

#include <rs/utility/Raii.h>

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
    : _deviceName{std::move(deviceName)}
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

    _callbacks = callbacks;
    _lastError.clear();

    ::snd_pcm_t* pcm = nullptr;

    // Open PCM device
    auto const openErr = ::snd_pcm_open(&pcm, _deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0);

    if (openErr < 0)
    {
      _lastError = "Failed to open ALSA device: " + _deviceName;
      PLAYBACK_LOG_ERROR("{}", _lastError);
      return false;
    }

    auto safePcm = rs::utility::makeUniquePtr<::snd_pcm_close>(pcm);

    // Set hardware parameters
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);

    if (auto const err = ::snd_pcm_hw_params_any(safePcm.get(), params); err < 0)
    {
      _lastError = "Failed to initialize ALSA hardware parameters";
      PLAYBACK_LOG_ERROR("{}", _lastError);
      return false;
    }

    // Set interleaved access
    if (auto const err = ::snd_pcm_hw_params_set_access(safePcm.get(), params, SND_PCM_ACCESS_RW_INTERLEAVED); err < 0)
    {
      _lastError = "Failed to set ALSA access mode";
      PLAYBACK_LOG_ERROR("{}", _lastError);
      return false;
    }

    // Set sample format (S16) - currently fixed for simplicity in this example
    auto const formatMask = ::SND_PCM_FORMAT_S16;
    if (auto const err = ::snd_pcm_hw_params_set_format(safePcm.get(), params, formatMask); err < 0)
    {
      _lastError = "Failed to set ALSA sample format";
      PLAYBACK_LOG_ERROR("{}", _lastError);
      return false;
    }

    // Set sample rate
    if (auto const err = ::snd_pcm_hw_params_set_rate(safePcm.get(), params, format.sampleRate, 0); err < 0)
    {
      _lastError = "Failed to set ALSA sample rate";
      PLAYBACK_LOG_ERROR("{}", _lastError);
      return false;
    }

    // Set channel count
    if (auto const err = ::snd_pcm_hw_params_set_channels(safePcm.get(), params, format.channels); err < 0)
    {
      _lastError = "Failed to set ALSA channel count";
      PLAYBACK_LOG_ERROR("{}", _lastError);
      return false;
    }

    // Apply hardware parameters
    if (auto const err = ::snd_pcm_hw_params(safePcm.get(), params); err < 0)
    {
      _lastError = "Failed to apply ALSA hardware parameters";
      PLAYBACK_LOG_ERROR("{}", _lastError);
      return false;
    }

    _format = format;
    _pcm.reset(safePcm.release());

    // Push initial graph
    if (_callbacks.onGraphChanged)
    {
      auto graph = AudioGraph{};
      
      // Stream Node
      auto streamNode = AudioNode{
        .id = "alsa-stream",
        .type = AudioNodeType::Stream,
        .name = "ALSA Stream",
        .format = _format
      };
      graph.nodes.push_back(std::move(streamNode));

      // Sink Node
      auto sinkNode = AudioNode{
        .id = "alsa-sink",
        .type = AudioNodeType::Sink,
        .name = _deviceName,
        .format = _format,
        .objectPath = _deviceName
      };
      graph.nodes.push_back(std::move(sinkNode));

      // Link
      graph.links.push_back({.sourceId = "alsa-stream", .destId = "alsa-sink"});

      _callbacks.onGraphChanged(_callbacks.userData, graph);
    }

    return true;
  }

  void AlsaExclusiveBackend::start()
  {
    if (!_pcm)
    {
      return;
    }

    ::snd_pcm_start(_pcm.get());
  }

  void AlsaExclusiveBackend::pause()
  {
    if (!_pcm)
    {
      return;
    }

    ::snd_pcm_pause(_pcm.get(), 1);
  }

  void AlsaExclusiveBackend::resume()
  {
    if (!_pcm)
    {
      return;
    }

    ::snd_pcm_pause(_pcm.get(), 0);
  }

  void AlsaExclusiveBackend::flush()
  {
    if (!_pcm)
    {
      return;
    }

    ::snd_pcm_drop(_pcm.get());
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

    ::snd_pcm_drain(_pcm.get());

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

    ::snd_pcm_drop(_pcm.get());
    ::snd_pcm_prepare(_pcm.get());
  }

  void AlsaExclusiveBackend::close()
  {
    _pcm.reset();
  }

  DeviceCapabilities AlsaExclusiveBackend::queryCapabilities() const
  {
    auto caps = DeviceCapabilities{};

    if (!_pcm)
    {
      return caps;
    }

    // Query available sample rates
    for (auto const targetRate : std::to_array({44100, 48000, 88200, 96000, 176400, 192000}))
    {
      if (::snd_pcm_hw_params_test_rate(_pcm.get(), nullptr, targetRate, 0) == 0)
      {
        caps.sampleRates.push_back(static_cast<std::uint32_t>(targetRate));
      }
    }

    // Query available bit depths
    for (auto const depth : std::to_array({16, 24, 32}))
    {
      auto const fmt = (depth == 16)   ? ::SND_PCM_FORMAT_S16
                       : (depth == 24) ? ::SND_PCM_FORMAT_S24
                                       : ::SND_PCM_FORMAT_S32;

      if (::snd_pcm_hw_params_test_format(_pcm.get(), nullptr, fmt) == 0)
      {
        caps.bitDepths.push_back(static_cast<std::uint8_t>(depth));
      }
    }

    // Query available channel counts
    for (auto const channels : std::to_array({1, 2, 4, 6, 8}))
    {
      if (::snd_pcm_hw_params_test_channels(_pcm.get(), nullptr, channels) == 0)
      {
        caps.channelCounts.push_back(static_cast<std::uint8_t>(channels));
      }
    }

    return caps;
  }

} // namespace app::playback
