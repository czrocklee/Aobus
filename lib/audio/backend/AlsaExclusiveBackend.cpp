// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/backend/AlsaExclusiveBackend.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Log.h>
#include <ao/utility/ThreadUtils.h>

#include <ao/utility/Raii.h>
#include <poll.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ao::audio::backend
{
  constexpr int kAlsaWaitTimeoutMs = 500;
  constexpr int kPollRetryDelayMs = 10;

  struct AlsaExclusiveBackend::Impl final
  {
    struct AlsaPcmDeleter final
    {
      void operator()(::snd_pcm_t* handle) const noexcept
      {
        if (handle != nullptr)
        {
          ::snd_pcm_close(handle);
        }
      }
    };
    using AlsaPcmPtr = std::unique_ptr<::snd_pcm_t, AlsaPcmDeleter>;

    std::string deviceName;
    ao::audio::Format format;
    ao::audio::RenderCallbacks callbacks;

    AlsaPcmPtr pcm;
    std::jthread thread;
    std::atomic<bool> paused{false};
    bool canPause = false;

    explicit Impl(std::string const& name)
      : deviceName{name}
    {
    }

    void playbackLoop(std::stop_token const& stopToken);
    void recoverFromXrun(int err) const;
  };

  void AlsaExclusiveBackend::Impl::playbackLoop(std::stop_token const& stopToken)
  {
    while (!stopToken.stop_requested())
    {
      if (paused.load(std::memory_order_relaxed))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollRetryDelayMs));
        continue;
      }

      auto const state = ::snd_pcm_state(pcm.get());

      if (state == SND_PCM_STATE_XRUN || state == SND_PCM_STATE_SUSPENDED)
      {
        recoverFromXrun(state == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE);
        continue;
      }

      // Only wait if the device is actually running.
      // In PREPARED state, wait might hang or fail on some drivers.
      if (state == SND_PCM_STATE_RUNNING)
      {
        if (::snd_pcm_wait(pcm.get(), kAlsaWaitTimeoutMs) < 0)
        {
          recoverFromXrun(-EPIPE);
          continue;
        }
      }

      ::snd_pcm_sframes_t avail = ::snd_pcm_avail_update(pcm.get());

      if (avail < 0)
      {
        recoverFromXrun(static_cast<int>(avail));
        continue;
      }

      if (avail == 0)
      {
        if (state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_PREPARED)
        {
          recoverFromXrun(-EPIPE);
        }
        else
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        continue;
      }

      ::snd_pcm_uframes_t frames = static_cast<::snd_pcm_uframes_t>(avail);
      ::snd_pcm_channel_area_t const* areas = nullptr;
      ::snd_pcm_uframes_t offset = 0;

      if (::snd_pcm_mmap_begin(pcm.get(), &areas, &offset, &frames) < 0)
      {
        recoverFromXrun(-EPIPE);
        continue;
      }

      auto* const dest = static_cast<std::byte*>(areas[0].addr) + (offset * (areas[0].step / 8));
      std::size_t const bytesToRead = static_cast<std::size_t>(frames) * (format.bitDepth / 8) * format.channels;
      std::size_t const bytesRead = callbacks.readPcm(callbacks.userData, ao::utility::bytes::view(dest, bytesToRead));

      if (bytesRead > 0)
      {
        auto const committed = ::snd_pcm_mmap_commit(
          pcm.get(), offset, bytesRead / static_cast<std::size_t>((format.bitDepth / 8) * format.channels));

        if (committed < 0)
        {
          recoverFromXrun(static_cast<int>(committed));
        }
        else if (callbacks.onPositionAdvanced != nullptr)
        {
          callbacks.onPositionAdvanced(callbacks.userData, static_cast<std::uint32_t>(committed));
        }
      }
      else
      {
        ::snd_pcm_mmap_commit(pcm.get(), offset, 0);

        if (callbacks.isSourceDrained(callbacks.userData))
        {
          ::snd_pcm_drain(pcm.get());

          if (callbacks.onDrainComplete != nullptr)
          {
            callbacks.onDrainComplete(callbacks.userData);
          }

          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  void AlsaExclusiveBackend::Impl::recoverFromXrun(int err) const
  {
    if (err == -EPIPE)
    {
      if (callbacks.onUnderrun != nullptr)
      {
        callbacks.onUnderrun(callbacks.userData);
      }

      ::snd_pcm_prepare(pcm.get());
    }
    else if (err == -ESTRPIPE)
    {
      while (::snd_pcm_resume(pcm.get()) == -EAGAIN)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollRetryDelayMs));
      }
    }
    else if (err == -ENODEV || err == -EBADF)
    {
      auto const errorMsg = std::format("ALSA Device Lost: {}", ::snd_strerror(err));

      if (callbacks.onBackendError != nullptr)
      {
        callbacks.onBackendError(callbacks.userData, errorMsg);
      }
    }
  }

  AlsaExclusiveBackend::AlsaExclusiveBackend(ao::audio::Device const& device, ao::audio::ProfileId const& /*profile*/)
    : _impl{std::make_unique<Impl>(device.id.value())}
  {
    AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Creating backend instance for device '{}'", _impl->deviceName);
  }

  AlsaExclusiveBackend::~AlsaExclusiveBackend()
  {
    AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Destroying backend instance");
    stop();
    close();
  }

  ao::Result<> AlsaExclusiveBackend::open(ao::audio::Format const& format, ao::audio::RenderCallbacks callbacks)
  {
    AUDIO_LOG_INFO("AlsaExclusiveBackend: Opening device '{}' with format {}Hz/{}b/{}ch",
                   _impl->deviceName,
                   format.sampleRate,
                   static_cast<int>(format.bitDepth),
                   static_cast<int>(format.channels));

    close();
    _impl->callbacks = callbacks;

    ::snd_pcm_t* pcm = nullptr;

    if (::snd_pcm_open(&pcm, _impl->deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0)
    {
      return ao::makeError(
        ao::Error::Code::DeviceNotFound, std::format("Failed to open ALSA device: {}", _impl->deviceName));
    }

    auto safePcm = Impl::AlsaPcmPtr(pcm);
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params); // macro

    if (::snd_pcm_hw_params_any(safePcm.get(), params) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to init ALSA hw params");
    }

    if (::snd_pcm_hw_params_set_access(safePcm.get(), params, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0)
    {
      return ao::makeError(ao::Error::Code::FormatRejected, "No mmap interleaved support");
    }

    auto alsaFormat = SND_PCM_FORMAT_S16_LE;

    if (format.bitDepth == 32)
    {
      alsaFormat = (format.validBits == 24) ? SND_PCM_FORMAT_S24_LE : SND_PCM_FORMAT_S32_LE;

      if (format.validBits == 24 && ::snd_pcm_hw_params_test_format(safePcm.get(), params, alsaFormat) != 0 &&
          ::snd_pcm_hw_params_test_format(safePcm.get(), params, SND_PCM_FORMAT_S32_LE) == 0)
      {
        alsaFormat = SND_PCM_FORMAT_S32_LE;
      }
    }
    else if (format.bitDepth == 24)
    {
      alsaFormat = SND_PCM_FORMAT_S24_3LE;
    }

    if (::snd_pcm_hw_params_set_format(safePcm.get(), params, alsaFormat) < 0)
    {
      return ao::makeError(ao::Error::Code::FormatRejected, "Bit depth not supported");
    }

    std::uint32_t rate = format.sampleRate;

    if (::snd_pcm_hw_params_set_rate_near(safePcm.get(), params, &rate, 0) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to set rate");
    }

    if (::snd_pcm_hw_params_set_channels(safePcm.get(), params, format.channels) < 0)
    {
      return ao::makeError(ao::Error::Code::FormatRejected, "Failed to set channels");
    }

    auto periods = std::uint32_t{4};
    auto periodSize = ::snd_pcm_uframes_t{1024};

    ::snd_pcm_hw_params_set_periods_near(safePcm.get(), params, &periods, 0);
    ::snd_pcm_hw_params_set_period_size_near(safePcm.get(), params, &periodSize, 0);

    if (::snd_pcm_hw_params(safePcm.get(), params) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to apply hw params");
    }

    _impl->canPause = (::snd_pcm_hw_params_can_pause(params) == 1);
    AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Device pause support: {}", _impl->canPause);

    ::snd_pcm_sw_params_t* swParams = nullptr;
    snd_pcm_sw_params_alloca(&swParams);

    if (::snd_pcm_sw_params_current(safePcm.get(), swParams) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to get sw params");
    }

    ::snd_pcm_sw_params_set_start_threshold(safePcm.get(), swParams, periodSize);
    ::snd_pcm_sw_params_set_avail_min(safePcm.get(), swParams, periodSize);

    if (::snd_pcm_sw_params(safePcm.get(), swParams) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to apply sw params");
    }

    _impl->format = format;
    _impl->format.sampleRate = rate;
    _impl->pcm = std::move(safePcm);

    if (_impl->callbacks.onRouteReady != nullptr)
    {
      _impl->callbacks.onRouteReady(_impl->callbacks.userData, _impl->deviceName);
    }

    return {};
  }

  void AlsaExclusiveBackend::reset()
  {
    close();
    _impl->callbacks = {};
  }

  void AlsaExclusiveBackend::start()
  {
    if (!_impl->pcm)
    {
      return;
    }

    if (!_impl->thread.joinable())
    {
      _impl->thread = std::jthread(
        [this](std::stop_token const& st)
        {
          ao::setCurrentThreadName("AlsaPlayback");
          _impl->playbackLoop(st);
        });
    }

    ::snd_pcm_start(_impl->pcm.get());
  }

  void AlsaExclusiveBackend::pause()
  {
    if (_impl->pcm)
    {
      _impl->paused = true;

      if (_impl->canPause)
      {
        ::snd_pcm_pause(_impl->pcm.get(), 1);
      }
      else
      {
        ::snd_pcm_drop(_impl->pcm.get());
      }
    }
  }

  void AlsaExclusiveBackend::resume()
  {
    if (_impl->pcm)
    {
      _impl->paused = false;
      int err = 0;

      if (_impl->canPause)
      {
        err = ::snd_pcm_pause(_impl->pcm.get(), 0);
      }

      if (!_impl->canPause || err < 0)
      {
        ::snd_pcm_prepare(_impl->pcm.get());
        ::snd_pcm_start(_impl->pcm.get());
      }
    }
  }

  void AlsaExclusiveBackend::flush()
  {
    if (_impl->pcm)
    {
      ::snd_pcm_drop(_impl->pcm.get());
      ::snd_pcm_prepare(_impl->pcm.get());
    }
  }

  void AlsaExclusiveBackend::drain()
  {
    if (!_impl->pcm)
    {
      if (_impl->callbacks.onDrainComplete != nullptr)
      {
        _impl->callbacks.onDrainComplete(_impl->callbacks.userData);
      }

      return;
    }

    ::snd_pcm_drain(_impl->pcm.get());

    if (_impl->callbacks.onDrainComplete != nullptr)
    {
      _impl->callbacks.onDrainComplete(_impl->callbacks.userData);
    }
  }

  void AlsaExclusiveBackend::stop()
  {
    _impl->thread.request_stop();

    if (_impl->thread.joinable() && std::this_thread::get_id() != _impl->thread.get_id())
    {
      _impl->thread.join();
    }

    if (_impl->pcm)
    {
      ::snd_pcm_drop(_impl->pcm.get());
      ::snd_pcm_prepare(_impl->pcm.get());
    }
  }

  void AlsaExclusiveBackend::close()
  {
    stop();
    _impl->pcm.reset();
  }

  void AlsaExclusiveBackend::setExclusiveMode([[maybe_unused]] bool exclusive)
  {
  }

  bool AlsaExclusiveBackend::isExclusiveMode() const noexcept
  {
    return true;
  }

  ao::audio::BackendId AlsaExclusiveBackend::backendId() const noexcept
  {
    return ao::audio::kBackendAlsa;
  }

  ao::audio::ProfileId AlsaExclusiveBackend::profileId() const noexcept
  {
    return ao::audio::kProfileExclusive;
  }
} // namespace ao::audio::backend
