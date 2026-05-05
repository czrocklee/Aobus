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
      void operator()(::snd_mixer_t* handle) const noexcept
      {
        if (handle != nullptr)
        {
          ::snd_mixer_close(handle);
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

    void playbackLoop(std::stop_token const& stopToken) const;
    void recoverFromXrun(int err) const;

    bool initMixer(::snd_pcm_t* pcm);
    void applyVolume(float vol) const;
    void applyMute(bool mute) const;
    float readVolume() const;

    ao::Result<> configureHwParams(::snd_pcm_t* pcm,
                                   ao::audio::Format& format,
                                   ::snd_pcm_format_t& alsaFormat,
                                   ::snd_pcm_uframes_t& periodSize);
    ao::Result<> configureSwParams(::snd_pcm_t* pcm, ::snd_pcm_uframes_t periodSize);

    bool waitForFrames(::snd_pcm_uframes_t periodSize) const;
    void handleXrun(int err) const;
    void commitFrames(::snd_pcm_uframes_t offset, ::snd_pcm_uframes_t framesRead) const;
  };

  void AlsaExclusiveBackend::Impl::playbackLoop(std::stop_token const& stopToken) const
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

    std::size_t const bytesPerFrame = (static_cast<std::size_t>(format.bitDepth) / 8) * format.channels;

    while (!stopToken.stop_requested())
    {
      if (paused.load(std::memory_order_relaxed))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollRetryDelayMs));
        continue;
      }

      if (!waitForFrames(periodSize))
      {
        continue;
      }

      ::snd_pcm_uframes_t frames = periodSize;
      ::snd_pcm_uframes_t offset = 0;
      ::snd_pcm_channel_area_t const* areas = nullptr;

      if (auto const err = ::snd_pcm_mmap_begin(pcm.get(), &areas, &offset, &frames); err < 0)
      {
        handleXrun(err);
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
        commitFrames(offset, framesRead);
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

  void AlsaExclusiveBackend::Impl::commitFrames(::snd_pcm_uframes_t offset, ::snd_pcm_uframes_t framesRead) const
  {
    auto const committed = ::snd_pcm_mmap_commit(pcm.get(), offset, framesRead);

    if (committed < 0)
    {
      handleXrun(static_cast<int>(committed));
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

  bool AlsaExclusiveBackend::Impl::waitForFrames(::snd_pcm_uframes_t periodSize) const
  {
    auto avail = ::snd_pcm_avail_update(pcm.get());

    if (avail < 0)
    {
      handleXrun(static_cast<int>(avail));
      return false;
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

      return false;
    }

    return true;
  }

  void AlsaExclusiveBackend::Impl::handleXrun(int err) const
  {
    recoverFromXrun(err);
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
      while (elem != nullptr)
      {
        if (::snd_mixer_selem_get_name(elem) == std::string_view{name})
        {
          mixerElem = elem;
          mixerElemName = name.data();
          break;
        }
        elem = ::snd_mixer_elem_next(elem);
      }

      if (mixerElem != nullptr)
      {
        break;
      }
    }

    if (mixerElem == nullptr)
    {
      return false;
    }

    ::snd_mixer_selem_get_playback_volume_range(mixerElem, &volMin, &volMax);
    hasDB = (::snd_mixer_selem_get_playback_dB_range(mixerElem, &dbMin, &dbMax) == 0);
    return true;
  }

  void AlsaExclusiveBackend::Impl::applyVolume(float vol) const
  {
    if (mixerElem == nullptr)
    {
      return;
    }
    float clamped = std::clamp(vol, 0.0F, 1.0F);

    if (hasDB)
    {
      long db = dbMin + static_cast<long>(static_cast<float>(dbMax - dbMin) * clamped);
      ::snd_mixer_selem_set_playback_dB_all(mixerElem, db, 0);
    }

    else
    {
      long val = volMin + static_cast<long>(static_cast<float>(volMax - volMin) * clamped);
      ::snd_mixer_selem_set_playback_volume_all(mixerElem, val);
    }
  }

  void AlsaExclusiveBackend::Impl::applyMute(bool mute) const
  {
    if (mixerElem == nullptr)
    {
      return;
    }

    ::snd_mixer_selem_set_playback_switch_all(mixerElem, mute ? 0 : 1);
  }

  float AlsaExclusiveBackend::Impl::readVolume() const
  {
    if (mixerElem == nullptr)
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
        return std::clamp(static_cast<float>(db - dbMin) / static_cast<float>(dbMax - dbMin), 0.0F, 1.0F);
      }
    }

    ::snd_mixer_selem_get_playback_volume(mixerElem, SND_MIXER_SCHN_MONO, &val);

    if (volMax != volMin)
    {
      return std::clamp(static_cast<float>(val - volMin) / static_cast<float>(volMax - volMin), 0.0F, 1.0F);
    }

    return 1.0F;
  }

  ao::Result<> AlsaExclusiveBackend::Impl::configureHwParams(::snd_pcm_t* pcm,
                                                             ao::audio::Format& format,
                                                             ::snd_pcm_format_t& alsaFormat,
                                                             ::snd_pcm_uframes_t& periodSize)
  {
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params); // macro

    if (::snd_pcm_hw_params_any(pcm, params) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to init ALSA hw params");
    }

    if (::snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0)
    {
      return ao::makeError(ao::Error::Code::FormatRejected, "No MMAP interleaved support");
    }

    alsaFormat = SND_PCM_FORMAT_S16_LE;

    if (format.bitDepth == 32)
    {
      alsaFormat = (format.validBits == 24) ? SND_PCM_FORMAT_S24_LE : SND_PCM_FORMAT_S32_LE;
    }
    else if (format.bitDepth == 24)
    {
      alsaFormat = SND_PCM_FORMAT_S24_3LE;
    }

    if (::snd_pcm_hw_params_set_format(pcm, params, alsaFormat) < 0)
    {
      if (format.bitDepth == 16)
      {
        alsaFormat = SND_PCM_FORMAT_S32_LE;

        if (::snd_pcm_hw_params_set_format(pcm, params, alsaFormat) < 0)
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

    if (::snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0) < 0)
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

    if (::snd_pcm_hw_params_set_channels(pcm, params, format.channels) < 0)
    {
      return ao::makeError(ao::Error::Code::FormatRejected, "Failed to set channels");
    }

    auto periods = std::uint32_t{4};
    ::snd_pcm_hw_params_set_periods_near(pcm, params, &periods, 0);

    periodSize = kDefaultPeriodSize;
    ::snd_pcm_hw_params_set_period_size_near(pcm, params, &periodSize, 0);

    if (::snd_pcm_hw_params(pcm, params) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to apply hw params");
    }

    canPause = (::snd_pcm_hw_params_can_pause(params) == 1);
    format.sampleRate = rate;
    return {};
  }

  ao::Result<> AlsaExclusiveBackend::Impl::configureSwParams(::snd_pcm_t* pcm, ::snd_pcm_uframes_t periodSize)
  {
    ::snd_pcm_sw_params_t* swParams = nullptr;
    snd_pcm_sw_params_alloca(&swParams);

    if (::snd_pcm_sw_params_current(pcm, swParams) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to get sw params");
    }

    ::snd_pcm_sw_params_set_start_threshold(pcm, swParams, periodSize);
    ::snd_pcm_sw_params_set_avail_min(pcm, swParams, periodSize);

    if (::snd_pcm_sw_params(pcm, swParams) < 0)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to apply sw params");
    }
    return {};
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
    auto currentFormat = format;
    auto alsaFormat = SND_PCM_FORMAT_S16_LE;
    ::snd_pcm_uframes_t periodSize = 0;

    if (auto const res = _impl->configureHwParams(safePcm.get(), currentFormat, alsaFormat, periodSize); !res)
    {
      return res;
    }

    if (auto const res = _impl->configureSwParams(safePcm.get(), periodSize); !res)
    {
      return res;
    }

    _impl->format = currentFormat;
    AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Device pause support: {}, Negotiated Rate: {}Hz",
                    _impl->canPause,
                    _impl->format.sampleRate);

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

  ao::Result<> AlsaExclusiveBackend::setProperty(ao::audio::PropertyId id, ao::audio::PropertyValue const& value)
  {
    if (id == ao::audio::PropertyId::Volume)
    {
      _impl->applyVolume(std::get<float>(value));
      return {};
    }

    if (id == ao::audio::PropertyId::Muted)
    {
      _impl->applyMute(std::get<bool>(value));
      return {};
    }

    return std::unexpected(ao::Error{.code = ao::Error::Code::NotSupported});
  }

  ao::Result<ao::audio::PropertyValue> AlsaExclusiveBackend::getProperty(ao::audio::PropertyId id) const
  {
    if (id == ao::audio::PropertyId::Volume)
    {
      return _impl->readVolume();
    }

    if (id == ao::audio::PropertyId::Muted)
    {
      if (_impl->mixerElem == nullptr)
      {
        return false;
      }

      int val = 0;
      ::snd_mixer_selem_get_playback_switch(_impl->mixerElem, SND_MIXER_SCHN_MONO, &val);
      return val == 0;
    }

    return std::unexpected(ao::Error{.code = ao::Error::Code::NotSupported});
  }

  ao::audio::PropertyInfo AlsaExclusiveBackend::queryProperty(ao::audio::PropertyId id) const noexcept
  {
    if (id == ao::audio::PropertyId::Volume || id == ao::audio::PropertyId::Muted)
    {
      bool const available = _impl != nullptr && _impl->mixerElem != nullptr;
      return {.canRead = true, .canWrite = true, .isAvailable = available, .emitsChangeNotifications = false};
    }

    return {};
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
