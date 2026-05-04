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
  constexpr std::size_t kDefaultPeriodSize = 1024;

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

    // --- Mixer members ---
    struct AlsaMixerDeleter
    {
      void operator()(::snd_mixer_t* h) const noexcept
      {
        if (h)
        {
          ::snd_mixer_close(h);
        }
      }
    };
    using AlsaMixerPtr = std::unique_ptr<::snd_mixer_t, AlsaMixerDeleter>;

    AlsaMixerPtr mixer;
    ::snd_mixer_elem_t* mixerElem = nullptr; // non-owning
    std::string mixerElemName;               // debug/log
    long volMin = 0, volMax = 100;
    bool hasDB = false;
    long dbMin = 0, dbMax = 0;

    explicit Impl(std::string const& name)
      : deviceName{name}
    {
    }

    void playbackLoop(std::stop_token const& stopToken);
    void recoverFromXrun(int err) const;

    bool initMixer(::snd_pcm_t* pcm);
    void applyVolume(float vol);
    void applyMute(bool mute);
    float readVolume() const;
  };

  void AlsaExclusiveBackend::Impl::playbackLoop(std::stop_token const& stopToken)
  {
    ::snd_pcm_uframes_t periodSize = 0;
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);

    if (::snd_pcm_hw_params_current(pcm.get(), params) == 0)
    {
      ::snd_pcm_hw_params_get_period_size(params, &periodSize, nullptr);
    }

    if (periodSize == 0)
    {
      periodSize = kDefaultPeriodSize;
    }

    std::size_t const bytesPerFrame = (format.bitDepth / 8) * format.channels;

    while (!stopToken.stop_requested())
    {
      if (paused.load(std::memory_order_relaxed))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollRetryDelayMs));
        continue;
      }

      auto avail = ::snd_pcm_avail_update(pcm.get());

      if (avail < 0)
      {
        recoverFromXrun(static_cast<int>(avail));
        continue;
      }

      if (static_cast<::snd_pcm_uframes_t>(avail) < periodSize)
      {
        // Only block on the pollfd when the device is truly running.
        // In PREPARED / XRUN / SUSPENDED states the fd may never signal,
        // so we fall back to a short sleep to keep the loop responsive.
        if (::snd_pcm_state(pcm.get()) == SND_PCM_STATE_RUNNING)
        {
          ::snd_pcm_wait(pcm.get(), kAlsaWaitTimeoutMs);
        }
        else
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        continue;
      }

      ::snd_pcm_uframes_t frames = periodSize;
      ::snd_pcm_uframes_t offset = 0;
      ::snd_pcm_channel_area_t const* areas = nullptr;

      if (auto const err = ::snd_pcm_mmap_begin(pcm.get(), &areas, &offset, &frames); err < 0)
      {
        recoverFromXrun(err);
        continue;
      }

      if (areas == nullptr || areas[0].addr == nullptr)
      {
        continue;
      }

      // Use the ALSA areas formula to compute the correct destination pointer,
      // respecting any hardware-specific first/step values.
      auto* const dst = static_cast<std::byte*>(areas[0].addr) + ((areas[0].first + offset * areas[0].step) / 8);
      auto const bytesToRead = static_cast<std::size_t>(frames) * bytesPerFrame;

      std::size_t const bytesRead = callbacks.readPcm(callbacks.userData, {dst, bytesToRead});

      if (bytesRead > 0)
      {
        auto const framesRead = static_cast<::snd_pcm_uframes_t>(bytesRead / (bytesPerFrame));
        auto const committed = ::snd_pcm_mmap_commit(pcm.get(), offset, framesRead);

        if (committed < 0)
        {
          recoverFromXrun(static_cast<int>(committed));
        }
        else
        {
          // XRUN recovery via snd_pcm_prepare leaves the device in
          // PREPARED state. If the auto-start threshold has been met
          // by now, explicitly kick the device into RUNNING.
          if (::snd_pcm_state(pcm.get()) == SND_PCM_STATE_PREPARED)
          {
            ::snd_pcm_start(pcm.get());
          }

          if (callbacks.onPositionAdvanced != nullptr)
          {
            callbacks.onPositionAdvanced(callbacks.userData, static_cast<std::uint32_t>(committed));
          }
        }
      }
      else
      {
        ::snd_pcm_mmap_commit(pcm.get(), offset, 0); // Release back to ALSA
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

  bool AlsaExclusiveBackend::Impl::initMixer(::snd_pcm_t* pcm)
  {
    ::snd_pcm_info_t* info = nullptr;
    snd_pcm_info_alloca(&info); // stack-alloc macro
    if (::snd_pcm_info(pcm, info) < 0)
    {
      return false;
    }
    int card = ::snd_pcm_info_get_card(info);

    ::snd_mixer_t* raw = nullptr;
    if (::snd_mixer_open(&raw, 0) < 0)
    {
      return false;
    }
    mixer.reset(raw);

    auto const cardStr = std::format("hw:{}", card);
    if (::snd_mixer_attach(raw, cardStr.c_str()) < 0)
    {
      return false;
    }
    if (::snd_mixer_selem_register(raw, nullptr, nullptr) < 0)
    {
      return false;
    }
    if (::snd_mixer_load(raw) < 0)
    {
      return false;
    }

    // Heuristic search — first match wins
    static constexpr auto kSelemNames = std::array<std::string_view, 4>{"Master", "PCM", "Digital", "Main"};
    for (auto const& name : kSelemNames)
    {
      auto* elem = ::snd_mixer_first_elem(raw);
      while (elem)
      {
        if (::snd_mixer_selem_get_name(elem) == std::string_view{name})
        {
          mixerElem = elem;
          mixerElemName = name.data();
          break;
        }
        elem = ::snd_mixer_elem_next(elem);
      }
      if (mixerElem)
      {
        break;
      }
    }
    if (!mixerElem)
    {
      return false;
    }

    ::snd_mixer_selem_get_playback_volume_range(mixerElem, &volMin, &volMax);
    hasDB = (::snd_mixer_selem_get_playback_dB_range(mixerElem, &dbMin, &dbMax) == 0);
    return true;
  }

  void AlsaExclusiveBackend::Impl::applyVolume(float vol)
  {
    if (!mixerElem)
    {
      return;
    }
    float clamped = std::clamp(vol, 0.0F, 1.0F);
    if (hasDB)
    {
      long db = dbMin + static_cast<long>((dbMax - dbMin) * clamped);
      ::snd_mixer_selem_set_playback_dB_all(mixerElem, db, 0);
    }
    else
    {
      long val = volMin + static_cast<long>((volMax - volMin) * clamped);
      ::snd_mixer_selem_set_playback_volume_all(mixerElem, val);
    }
  }

  void AlsaExclusiveBackend::Impl::applyMute(bool mute)
  {
    if (!mixerElem)
    {
      return;
    }
    ::snd_mixer_selem_set_playback_switch_all(mixerElem, mute ? 0 : 1);
  }

  float AlsaExclusiveBackend::Impl::readVolume() const
  {
    if (!mixerElem)
    {
      return 1.0F;
    }
    long val = 0;
    if (hasDB)
    {
      long db = 0;
      ::snd_mixer_selem_get_playback_dB(mixerElem, SND_MIXER_SCHN_MONO, &db);
      if (dbMax != dbMin)
      {
        return std::clamp(static_cast<float>(db - dbMin) / (dbMax - dbMin), 0.0F, 1.0F);
      }
    }
    ::snd_mixer_selem_get_playback_volume(mixerElem, SND_MIXER_SCHN_MONO, &val);
    if (volMax != volMin)
    {
      return std::clamp(static_cast<float>(val - volMin) / (volMax - volMin), 0.0F, 1.0F);
    }
    return 1.0F;
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
      return ao::makeError(ao::Error::Code::FormatRejected, "No MMAP interleaved support");
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
      if (format.bitDepth == 16)
      {
        alsaFormat = SND_PCM_FORMAT_S32_LE;

        if (::snd_pcm_hw_params_set_format(safePcm.get(), params, alsaFormat) < 0)
        {
          return ao::makeError(ao::Error::Code::FormatRejected, "Hardware supports neither S16_LE nor S32_LE");
        }
      }
      else
      {
        return ao::makeError(ao::Error::Code::FormatRejected, "Format not supported by hardware");
      }
    }

    std::uint32_t rate = format.sampleRate;

    if (::snd_pcm_hw_params_set_rate_near(safePcm.get(), params, &rate, 0) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to set rate");
    }

    if (rate != format.sampleRate)
    {
      AUDIO_LOG_INFO("AlsaExclusiveBackend: Rate mismatch! Requested: {}Hz, Got: {}Hz. Pitch shift expected if engine "
                     "is not notified.",
                     format.sampleRate,
                     rate);
    }

    if (::snd_pcm_hw_params_set_channels(safePcm.get(), params, format.channels) < 0)
    {
      return ao::makeError(ao::Error::Code::FormatRejected, "Failed to set channels");
    }

    auto periods = std::uint32_t{4};
    ::snd_pcm_hw_params_set_periods_near(safePcm.get(), params, &periods, 0);

    ::snd_pcm_uframes_t periodSize = kDefaultPeriodSize;
    ::snd_pcm_hw_params_set_period_size_near(safePcm.get(), params, &periodSize, 0);

    if (::snd_pcm_hw_params(safePcm.get(), params) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to apply hw params");
    }

    _impl->canPause = (::snd_pcm_hw_params_can_pause(params) == 1);
    AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Device pause support: {}, Negotiated Rate: {}Hz", _impl->canPause, rate);

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

    // When the ALSA hardware forces a wider sample container (e.g. S16_LE
    // rejected in favour of S32_LE), the Engine must be told so its PCM
    // converter produces data that matches the MMAP buffer layout.
    auto const hwBitDepth = [&] -> std::uint8_t
    {
      if (alsaFormat == SND_PCM_FORMAT_S32_LE || alsaFormat == SND_PCM_FORMAT_S24_LE)
      {
        return 32;
      }

      if (alsaFormat == SND_PCM_FORMAT_S24_3LE)
      {
        return 24;
      }

      return 16;
    }();

    if (hwBitDepth != format.bitDepth)
    {
      _impl->format.bitDepth = hwBitDepth;
      _impl->format.validBits = format.bitDepth;

      if (_impl->callbacks.onFormatChanged != nullptr)
      {
        _impl->callbacks.onFormatChanged(_impl->callbacks.userData, _impl->format);
      }
    }

    _impl->pcm = std::move(safePcm);

    if (_impl->callbacks.onRouteReady != nullptr)
    {
      _impl->callbacks.onRouteReady(_impl->callbacks.userData, _impl->deviceName);
    }

    if (!_impl->initMixer(_impl->pcm.get()))
    {
      AUDIO_LOG_DEBUG("AlsaExclusiveBackend: No hardware mixer found for device '{}'", _impl->deviceName);
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
    _impl->mixer.reset();
    _impl->mixerElem = nullptr;
  }

  void AlsaExclusiveBackend::setExclusiveMode(bool /*exclusive*/)
  {
  }

  bool AlsaExclusiveBackend::isExclusiveMode() const noexcept
  {
    return true;
  }

  void AlsaExclusiveBackend::setVolume(float volume)
  {
    _impl->applyVolume(volume);
  }

  float AlsaExclusiveBackend::getVolume() const
  {
    return _impl->readVolume();
  }

  void AlsaExclusiveBackend::setMuted(bool muted)
  {
    _impl->applyMute(muted);
  }

  bool AlsaExclusiveBackend::isMuted() const
  {
    if (!_impl->mixerElem)
    {
      return false;
    }
    int val = 0;
    ::snd_mixer_selem_get_playback_switch(_impl->mixerElem, SND_MIXER_SCHN_MONO, &val);
    return val == 0;
  }

  bool AlsaExclusiveBackend::isVolumeAvailable() const
  {
    return _impl && _impl->mixerElem != nullptr;
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
