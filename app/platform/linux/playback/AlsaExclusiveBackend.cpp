// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/AlsaExclusiveBackend.h"
#include <rs/audio/BackendTypes.h>
#include <rs/utility/ByteView.h>
#include <rs/utility/Log.h>
#include <rs/utility/ThreadUtils.h>

#include <poll.h>
#include <rs/utility/Raii.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

namespace app::playback
{
  constexpr int kAlsaWaitTimeoutMs = 500;
  constexpr int kPollRetryDelayMs = 10;

  AlsaExclusiveBackend::AlsaExclusiveBackend(rs::audio::AudioDevice const& device)
    : _deviceName{device.id}
  {
    PLAYBACK_LOG_DEBUG("AlsaExclusiveBackend: Creating backend instance for device '{}'", _deviceName);
  }

  AlsaExclusiveBackend::~AlsaExclusiveBackend()
  {
    PLAYBACK_LOG_DEBUG("AlsaExclusiveBackend: Destroying backend instance");
    stop();
    close();
  }

  rs::Result<> AlsaExclusiveBackend::open(rs::audio::AudioFormat const& format,
                                          rs::audio::RenderCallbacks callbacks)
  {
    PLAYBACK_LOG_INFO("AlsaExclusiveBackend: Opening device '{}' with format {}Hz/{}b/{}ch",
                      _deviceName,
                      format.sampleRate,
                      static_cast<int>(format.bitDepth),
                      static_cast<int>(format.channels));

    close();
    _callbacks = callbacks;

    ::snd_pcm_t* pcm = nullptr;
    if (::snd_pcm_open(&pcm, _deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0)
    {
      return rs::makeError(rs::Error::Code::DeviceNotFound, std::format("Failed to open ALSA device: {}", _deviceName));
    }

    auto safePcm = AlsaPcmPtr(pcm);
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params); // macro
    if (::snd_pcm_hw_params_any(safePcm.get(), params) < 0)
    {
      return rs::makeError(rs::Error::Code::InitFailed, "Failed to init ALSA hw params");
    }
    if (::snd_pcm_hw_params_set_access(safePcm.get(), params, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0)
    {
      return rs::makeError(rs::Error::Code::FormatRejected, "No mmap interleaved support");
    }

    auto alsaFormat = SND_PCM_FORMAT_S16_LE;
    if (format.bitDepth == 32)
    {
      alsaFormat = (format.validBits == 24) ? SND_PCM_FORMAT_S24_LE : SND_PCM_FORMAT_S32_LE;
    }
    else if (format.bitDepth == 24)
    {
      alsaFormat = SND_PCM_FORMAT_S24_3LE;
    }

    if (::snd_pcm_hw_params_set_format(safePcm.get(), params, alsaFormat) < 0)
    {
      return rs::makeError(rs::Error::Code::FormatRejected, "Bit depth not supported");
    }
    std::uint32_t rate = format.sampleRate;
    if (::snd_pcm_hw_params_set_rate_near(safePcm.get(), params, &rate, 0) < 0)
    {
      return rs::makeError(rs::Error::Code::InitFailed, "Failed to set rate");
    }
    if (::snd_pcm_hw_params_set_channels(safePcm.get(), params, format.channels) < 0)
    {
      return rs::makeError(rs::Error::Code::FormatRejected, "Failed to set channels");
    }

    std::uint32_t periods = 4;
    snd_pcm_uframes_t periodSize = 1024;
    ::snd_pcm_hw_params_set_periods_near(safePcm.get(), params, &periods, 0);
    ::snd_pcm_hw_params_set_period_size_near(safePcm.get(), params, &periodSize, 0);
    if (::snd_pcm_hw_params(safePcm.get(), params) < 0)
    {
      return rs::makeError(rs::Error::Code::InitFailed, "Failed to apply hw params");
    }

    ::snd_pcm_sw_params_t* swParams = nullptr;
    snd_pcm_sw_params_alloca(&swParams);
    if (::snd_pcm_sw_params_current(safePcm.get(), swParams) < 0)
    {
      return rs::makeError(rs::Error::Code::InitFailed, "Failed to get sw params");
    }
    ::snd_pcm_sw_params_set_start_threshold(safePcm.get(), swParams, periodSize);
    ::snd_pcm_sw_params_set_avail_min(safePcm.get(), swParams, periodSize);
    if (::snd_pcm_sw_params(safePcm.get(), swParams) < 0)
    {
      return rs::makeError(rs::Error::Code::InitFailed, "Failed to apply sw params");
    }

    _format = format;
    _format.sampleRate = rate;
    _pcm = std::move(safePcm);

    if (_callbacks.onRouteReady != nullptr)
    {
      _callbacks.onRouteReady(_callbacks.userData, _deviceName);
    }
    return {};
  }

  void AlsaExclusiveBackend::playbackLoop(std::stop_token const& stopToken)
  {
    while (!stopToken.stop_requested())
    {
      if (_paused.load(std::memory_order_relaxed))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollRetryDelayMs));
        continue;
      }
      if (::snd_pcm_wait(_pcm.get(), kAlsaWaitTimeoutMs) < 0)
      {
        recoverFromXrun(-EPIPE);
        continue;
      }
      ::snd_pcm_sframes_t avail = ::snd_pcm_avail_update(_pcm.get());
      if (avail < 0)
      {
        recoverFromXrun(static_cast<int>(avail));
        continue;
      }
      if (avail == 0)
      {
        continue;
      }

      ::snd_pcm_uframes_t frames = static_cast<::snd_pcm_uframes_t>(avail);
      ::snd_pcm_channel_area_t const* areas = nullptr;
      ::snd_pcm_uframes_t offset = 0;
      if (::snd_pcm_mmap_begin(_pcm.get(), &areas, &offset, &frames) < 0)
      {
        recoverFromXrun(-EPIPE);
        continue;
      }

      auto* const dest = static_cast<std::byte*>(areas[0].addr) + (offset * (areas[0].step / 8));
      std::size_t const bytesToRead = static_cast<std::size_t>(frames) * (_format.bitDepth / 8) * _format.channels;
      std::size_t const bytesRead =
        _callbacks.readPcm(_callbacks.userData, rs::utility::bytes::view(dest, bytesToRead));

      if (bytesRead > 0)
      {
        auto committed = ::snd_pcm_mmap_commit(
          _pcm.get(), offset, bytesRead / static_cast<std::size_t>((_format.bitDepth / 8) * _format.channels));

        if (committed < 0)
        {
          recoverFromXrun(static_cast<int>(committed));
        }

        else if (_callbacks.onPositionAdvanced != nullptr)
        {
          _callbacks.onPositionAdvanced(_callbacks.userData, static_cast<std::uint32_t>(committed));
        }
      }
      else
      {
        ::snd_pcm_mmap_commit(_pcm.get(), offset, 0);

        if (_callbacks.isSourceDrained(_callbacks.userData))
        {
          ::snd_pcm_drain(_pcm.get());

          if (_callbacks.onDrainComplete != nullptr)
          {
            _callbacks.onDrainComplete(_callbacks.userData);
          }

          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  void AlsaExclusiveBackend::recoverFromXrun(int err)
  {
    if (err == -EPIPE)
    {
      if (_callbacks.onUnderrun != nullptr)
      {
        _callbacks.onUnderrun(_callbacks.userData);
      }
      ::snd_pcm_prepare(_pcm.get());
    }
    else if (err == -ESTRPIPE)
    {
      while (::snd_pcm_resume(_pcm.get()) == -EAGAIN)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollRetryDelayMs));
      }
    }
    else if (err == -ENODEV || err == -EBADF)
    {
      auto errorMsg = std::format("ALSA Device Lost: {}", ::snd_strerror(err));
      if (_callbacks.onBackendError != nullptr)
      {
        _callbacks.onBackendError(_callbacks.userData, errorMsg);
      }
    }
  }

  void AlsaExclusiveBackend::reset()
  {
    close();
    _callbacks = {};
  }

  void AlsaExclusiveBackend::start()
  {
    if (!_pcm)
    {
      return;
    }
    if (!_thread.joinable())
    {
      _thread = std::jthread(
        [this](std::stop_token const& st)
        {
          rs::setCurrentThreadName("AlsaPlayback");
          playbackLoop(st);
        });
    }
    ::snd_pcm_start(_pcm.get());
  }

  void AlsaExclusiveBackend::pause()
  {
    if (_pcm)
    {
      _paused = true;
      ::snd_pcm_pause(_pcm.get(), 1);
    }
  }
  void AlsaExclusiveBackend::resume()
  {
    if (_pcm)
    {
      _paused = false;
      ::snd_pcm_pause(_pcm.get(), 0);
    }
  }
  void AlsaExclusiveBackend::flush()
  {
    if (_pcm)
    {
      ::snd_pcm_drop(_pcm.get());
      ::snd_pcm_prepare(_pcm.get());
    }
  }
  void AlsaExclusiveBackend::drain()
  {
    if (!_pcm)
    {
      if (_callbacks.onDrainComplete != nullptr)
      {
        _callbacks.onDrainComplete(_callbacks.userData);
      }
      return;
    }
    ::snd_pcm_drain(_pcm.get());
    if (_callbacks.onDrainComplete != nullptr)
    {
      _callbacks.onDrainComplete(_callbacks.userData);
    }
  }
  void AlsaExclusiveBackend::stop()
  {
    _thread.request_stop();
    if (_thread.joinable() && std::this_thread::get_id() != _thread.get_id())
    {
      _thread.join();
    }
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
  void AlsaExclusiveBackend::setExclusiveMode([[maybe_unused]] bool exclusive)
  {
  }
  bool AlsaExclusiveBackend::isExclusiveMode() const noexcept
  {
    return true;
  }
  rs::audio::BackendKind AlsaExclusiveBackend::kind() const noexcept
  {
    return rs::audio::BackendKind::AlsaExclusive;
  }
} // namespace app::playback
