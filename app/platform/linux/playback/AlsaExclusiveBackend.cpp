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

    auto safePcm = AlsaPcmPtr(pcm);

    // 1. Hardware Parameters
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);

    if (::snd_pcm_hw_params_any(safePcm.get(), params) < 0)
    {
      _lastError = "Failed to initialize ALSA hardware parameters";
      return false;
    }

    // Set mmap interleaved access
    if (::snd_pcm_hw_params_set_access(safePcm.get(), params, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0)
    {
      _lastError = "ALSA device does not support mmap interleaved access";
      return false;
    }

    // Set sample format
    auto const alsaFormat = (format.bitDepth == 16) ? SND_PCM_FORMAT_S16_LE :
                            (format.bitDepth == 24) ? SND_PCM_FORMAT_S24_3LE :
                                                      SND_PCM_FORMAT_S32_LE;

    if (::snd_pcm_hw_params_set_format(safePcm.get(), params, alsaFormat) < 0)
    {
      _lastError = "ALSA device does not support requested bit depth";
      return false;
    }

    // Set sample rate
    unsigned int rate = format.sampleRate;
    if (::snd_pcm_hw_params_set_rate_near(safePcm.get(), params, &rate, 0) < 0)
    {
      _lastError = "Failed to set ALSA sample rate";
      return false;
    }

    // Set channels
    if (::snd_pcm_hw_params_set_channels(safePcm.get(), params, format.channels) < 0)
    {
      _lastError = "Failed to set ALSA channels";
      return false;
    }

    // Set buffer size (aim for 4 periods of 1024 frames)
    unsigned int periods = 4;
    snd_pcm_uframes_t periodSize = 1024;
    if (::snd_pcm_hw_params_set_periods_near(safePcm.get(), params, &periods, 0) < 0)
    {
       _lastError = "Failed to set ALSA periods";
       return false;
    }
    if (::snd_pcm_hw_params_set_period_size_near(safePcm.get(), params, &periodSize, 0) < 0)
    {
       _lastError = "Failed to set ALSA period size";
       return false;
    }

    if (::snd_pcm_hw_params(safePcm.get(), params) < 0)
    {
      _lastError = "Failed to apply ALSA hardware parameters";
      return false;
    }

    // 2. Software Parameters
    ::snd_pcm_sw_params_t* swParams = nullptr;
    snd_pcm_sw_params_alloca(&swParams);

    if (::snd_pcm_sw_params_current(safePcm.get(), swParams) < 0)
    {
      _lastError = "Failed to get ALSA software parameters";
      return false;
    }

    // Start threshold: start as soon as we have one period ready
    if (::snd_pcm_sw_params_set_start_threshold(safePcm.get(), swParams, periodSize) < 0)
    {
       _lastError = "Failed to set ALSA start threshold";
       return false;
    }

    // Min availability: wake us when at least one period is free
    if (::snd_pcm_sw_params_set_avail_min(safePcm.get(), swParams, periodSize) < 0)
    {
       _lastError = "Failed to set ALSA avail min";
       return false;
    }

    if (::snd_pcm_sw_params(safePcm.get(), swParams) < 0)
    {
      _lastError = "Failed to apply ALSA software parameters";
      return false;
    }

    _format = format;
    _format.sampleRate = rate; // Update with actual rate from device
    _pcm = std::move(safePcm);

    PLAYBACK_LOG_INFO("ALSA backend opened device '{}' [mmap] format: {}Hz/{}b/{}ch", 
                       _deviceName, _format.sampleRate, (int)_format.bitDepth, (int)_format.channels);

    // Push initial graph
    if (_callbacks.onGraphChanged)
    {
      auto graph = AudioGraph{};
      graph.nodes.push_back({
        .id = "alsa-stream", 
        .type = AudioNodeType::Stream, 
        .name = "ALSA Stream", 
        .format = _format
      });
      graph.nodes.push_back({
        .id = "alsa-sink", 
        .type = AudioNodeType::Sink, 
        .name = _deviceName, 
        .format = _format, 
        .volumeNotUnity = false, // Exclusive hardware access usually implies no soft volume
        .isMuted = false,
        .objectPath = _deviceName
      });
      graph.links.push_back({
        .sourceId = "alsa-stream", 
        .destId = "alsa-sink",
        .isActive = true
      });
      _callbacks.onGraphChanged(_callbacks.userData, graph);
    }

    return true;
  }

  void AlsaExclusiveBackend::playbackLoop(std::stop_token stopToken)
  {
    PLAYBACK_LOG_INFO("ALSA playback thread started");

    while (!stopToken.stop_requested())
    {
      if (_paused.load(std::memory_order_relaxed))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      // Wait for the device to be ready for more data
      int const waitErr = ::snd_pcm_wait(_pcm.get(), 500);
      if (waitErr < 0)
      {
        recoverFromXrun(waitErr);
        continue;
      }

      // How many frames are available to write?
      ::snd_pcm_sframes_t avail = ::snd_pcm_avail_update(_pcm.get());
      if (avail < 0)
      {
        recoverFromXrun(static_cast<int>(avail));
        continue;
      }

      if (avail == 0) continue;

      // Start mmap cycle
      ::snd_pcm_uframes_t frames = static_cast<::snd_pcm_uframes_t>(avail);
      ::snd_pcm_channel_area_t const* areas = nullptr;
      ::snd_pcm_uframes_t offset = 0;

      int const beginErr = ::snd_pcm_mmap_begin(_pcm.get(), &areas, &offset, &frames);
      if (beginErr < 0)
      {
        recoverFromXrun(beginErr);
        continue;
      }

      // Calculate destination memory address
      // For interleaved access, all channels are in areas[0]
      char* dest = static_cast<char*>(areas[0].addr) + (offset * (areas[0].step / 8));
      std::size_t const bytesToRead = frames * (_format.bitDepth / 8) * _format.channels;

      // Invoke RockStudio engine to write directly to mmap buffer
      std::size_t const bytesRead = _callbacks.readPcm(_callbacks.userData, {reinterpret_cast<std::byte*>(dest), bytesToRead});

      if (bytesRead > 0)
      {
        ::snd_pcm_sframes_t const framesRead = bytesRead / ((_format.bitDepth / 8) * _format.channels);
        
        // Tell ALSA how many frames we actually wrote
        ::snd_pcm_sframes_t const committed = ::snd_pcm_mmap_commit(_pcm.get(), offset, framesRead);
        if (committed < 0)
        {
          recoverFromXrun(static_cast<int>(committed));
        }
        else
        {
          _callbacks.onPositionAdvanced(_callbacks.userData, static_cast<std::uint32_t>(committed));
        }
      }
      else
      {
        // No more data from source (EOF)
        ::snd_pcm_mmap_commit(_pcm.get(), offset, 0);
        
        if (_callbacks.isSourceDrained(_callbacks.userData))
        {
          PLAYBACK_LOG_INFO("ALSA source drained, stopping playback");
          break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    PLAYBACK_LOG_INFO("ALSA playback thread exiting");
  }

  void AlsaExclusiveBackend::recoverFromXrun(int err)
  {
    if (err == -EPIPE)
    {
      PLAYBACK_LOG_WARN("ALSA underrun (XRUN) detected!");
      _callbacks.onUnderrun(_callbacks.userData);
      ::snd_pcm_prepare(_pcm.get());
    }
    else if (err == -ESTRPIPE)
    {
      PLAYBACK_LOG_WARN("ALSA device suspended, waiting for resume...");
      while (::snd_pcm_resume(_pcm.get()) == -EAGAIN)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    else if (err < 0)
    {
      PLAYBACK_LOG_ERROR("ALSA playback error: {}", ::snd_strerror(err));
    }
  }

  void AlsaExclusiveBackend::start()
  {
    if (!_pcm) return;

    _paused = false;
    
    if (!_thread.joinable())
    {
      _thread = std::jthread([this](std::stop_token st) { playbackLoop(st); });
    }

    ::snd_pcm_start(_pcm.get());
  }

  void AlsaExclusiveBackend::pause()
  {
    if (!_pcm) return;
    _paused = true;
    ::snd_pcm_pause(_pcm.get(), 1);
  }

  void AlsaExclusiveBackend::resume()
  {
    if (!_pcm) return;
    _paused = false;
    ::snd_pcm_pause(_pcm.get(), 0);
  }

  void AlsaExclusiveBackend::flush()
  {
    if (!_pcm) return;
    ::snd_pcm_drop(_pcm.get());
    ::snd_pcm_prepare(_pcm.get());
  }

  void AlsaExclusiveBackend::drain()
  {
    if (!_pcm)
    {
      if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData);
      return;
    }

    ::snd_pcm_drain(_pcm.get());
    if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData);
  }

  void AlsaExclusiveBackend::stop()
  {
    _thread.request_stop();
    if (_thread.joinable()) _thread.join();
    
    if (_pcm)
    {
      ::snd_pcm_drop(_pcm.get());
      ::snd_pcm_prepare(_pcm.get());
    }
  }

  void AlsaExclusiveBackend::close()
  {
    stop();
    _pcm.reset();
  }

  DeviceCapabilities AlsaExclusiveBackend::queryCapabilities() const
  {
    auto caps = DeviceCapabilities{};

    if (!_pcm)
    {
      return caps;
    }

    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    if (::snd_pcm_hw_params_any(_pcm.get(), params) < 0)
    {
      return caps;
    }

    // Query available sample rates
    for (auto const targetRate : std::to_array({44100, 48000, 88200, 96000, 176400, 192000}))
    {
      if (::snd_pcm_hw_params_test_rate(_pcm.get(), params, targetRate, 0) == 0)
      {
        caps.sampleRates.push_back(static_cast<std::uint32_t>(targetRate));
      }
    }

    // Query available bit depths
    for (auto const depth : std::to_array({16, 24, 32}))
    {
      auto const fmt = (depth == 16)   ? ::SND_PCM_FORMAT_S16_LE
                       : (depth == 24) ? ::SND_PCM_FORMAT_S24_3LE
                                       : ::SND_PCM_FORMAT_S32_LE;

      if (::snd_pcm_hw_params_test_format(_pcm.get(), params, fmt) == 0)
      {
        caps.bitDepths.push_back(static_cast<std::uint8_t>(depth));
      }
    }

    // Query available channel counts
    for (auto const channels : std::to_array({1, 2, 4, 6, 8}))
    {
      if (::snd_pcm_hw_params_test_channels(_pcm.get(), params, channels) == 0)
      {
        caps.channelCounts.push_back(static_cast<std::uint8_t>(channels));
      }
    }

    return caps;
  }

} // namespace app::playback
