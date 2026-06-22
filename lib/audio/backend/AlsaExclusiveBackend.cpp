// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/Property.h>
#include <ao/audio/backend/AlsaExclusiveBackend.h>
#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
#include <ao/audio/backend/detail/AlsaPcmError.h>
#include <ao/audio/backend/detail/AlsaPcmVolume.h>
#include <ao/utility/Log.h>
#include <ao/utility/ThreadUtils.h>

#include <poll.h>

#include <cerrno>
#include <cmath>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  constexpr auto kAlsaWaitTimeout = std::chrono::milliseconds{500};
  constexpr auto kPollRetryDelay = std::chrono::milliseconds{10};
  constexpr std::size_t kDefaultPeriodSize = 1024;

  namespace
  {
    using AlsaMixerLevel = decltype(std::lround(0.0));

    struct AlsaMixerCandidate final
    {
      ::snd_mixer_elem_t* elem = nullptr;
      std::string name{};
      std::int32_t rank = 0;
    };

    struct AlsaMixerRange final
    {
      std::ptrdiff_t min = 0;
      std::ptrdiff_t max = 0;
    };

    constexpr std::int32_t kRankMaster = 1;
    constexpr std::int32_t kRankPcm = 2;
    constexpr std::int32_t kRankDigital = 3;
    constexpr std::int32_t kRankMain = 4;
    constexpr std::int32_t kRankLowest = 5;

    std::int32_t mixerRank(std::string_view name) noexcept
    {
      if (name == "Master")
      {
        return kRankMaster;
      }

      if (name == "PCM")
      {
        return kRankPcm;
      }

      if (name == "Digital")
      {
        return kRankDigital;
      }

      if (name == "Main")
      {
        return kRankMain;
      }

      return kRankLowest;
    }

    AlsaMixerLevel toAlsaMixerLevel(std::ptrdiff_t value) noexcept
    {
      return static_cast<AlsaMixerLevel>(value);
    }

    bool setPlaybackVolumeAll(::snd_mixer_elem_t* elem, std::ptrdiff_t value)
    {
      return ::snd_mixer_selem_set_playback_volume_all(elem, toAlsaMixerLevel(value)) == 0;
    }

    std::optional<std::ptrdiff_t> optPlaybackVolume(::snd_mixer_elem_t* elem, ::snd_mixer_selem_channel_id_t channel)
    {
      long value = 0L;

      if (::snd_mixer_selem_get_playback_volume(elem, channel, &value) < 0)
      {
        return std::nullopt;
      }

      return static_cast<std::ptrdiff_t>(value);
    }

    std::optional<AlsaMixerRange> optPlaybackVolumeRange(::snd_mixer_elem_t* elem)
    {
      long min = 0L;
      long max = 0L;

      if (::snd_mixer_selem_get_playback_volume_range(elem, &min, &max) < 0 || max <= min)
      {
        return std::nullopt;
      }

      return AlsaMixerRange{.min = static_cast<std::ptrdiff_t>(min), .max = static_cast<std::ptrdiff_t>(max)};
    }

    std::optional<std::ptrdiff_t> optProbeTarget(AlsaMixerRange const& range, std::ptrdiff_t original) noexcept
    {
      auto const target = (original > range.min) ? original - 1 : original + 1;

      if (target < range.min || target > range.max)
      {
        return std::nullopt;
      }

      return target;
    }

    std::vector<AlsaMixerCandidate> collectMixerCandidates(::snd_mixer_t* mixer)
    {
      auto candidates = std::vector<AlsaMixerCandidate>{};

      for (auto* elem = ::snd_mixer_first_elem(mixer); elem != nullptr; elem = ::snd_mixer_elem_next(elem))
      {
        if (::snd_mixer_selem_is_active(elem) == 0 || ::snd_mixer_selem_has_playback_volume(elem) == 0)
        {
          continue;
        }

        auto name = std::string{::snd_mixer_selem_get_name(elem)};
        candidates.push_back({.elem = elem, .name = name, .rank = mixerRank(name)});
      }

      std::ranges::sort(candidates, {}, &AlsaMixerCandidate::rank);
      return candidates;
    }

    bool verifyMixerWriteReadback(AlsaMixerCandidate const& candidate, AlsaMixerRange const& range)
    {
      auto const optOriginal = optPlaybackVolume(candidate.elem, SND_MIXER_SCHN_FRONT_LEFT);

      if (!optOriginal)
      {
        return false;
      }

      auto const optTarget = optProbeTarget(range, *optOriginal);

      if (!optTarget)
      {
        return false;
      }

      if (!setPlaybackVolumeAll(candidate.elem, *optTarget))
      {
        return false;
      }

      auto const optReadback = optPlaybackVolume(candidate.elem, SND_MIXER_SCHN_FRONT_LEFT);

      if (!optReadback || *optReadback != *optTarget)
      {
        setPlaybackVolumeAll(candidate.elem, *optOriginal);
        return false;
      }

      if (!setPlaybackVolumeAll(candidate.elem, *optOriginal))
      {
        AUDIO_LOG_WARN("AlsaExclusiveBackend: Mixer restore failed for '{}'", candidate.name);
        return false;
      }

      return true;
    }

    class AlsaMixerController final
    {
    public:
      AlsaMixerController() = default;

      bool init(::snd_pcm_t* pcm)
      {
        auto const lock = std::scoped_lock{_handleMutex};

        if (!openMixer(pcm))
        {
          _volumeMode = detail::AlsaVolumeControlMode::SoftwareGain;
          return false;
        }

        for (auto const& candidate : collectMixerCandidates(_mixerPtr.get()))
        {
          if (tryUseMixerCandidate(candidate))
          {
            AUDIO_LOG_INFO("AlsaExclusiveBackend: Hardware mixer '{}' selected and verified", _mixerElemName);
            return true;
          }
        }

        _volumeMode = detail::AlsaVolumeControlMode::SoftwareGain;
        AUDIO_LOG_INFO("AlsaExclusiveBackend: No functional hardware mixer found, using software fallback");
        return false;
      }

      void close()
      {
        auto const lock = std::scoped_lock{_handleMutex};
        _mixerPtr.reset();
        _mixerElem = nullptr;
        _volumeMode = detail::AlsaVolumeControlMode::Unavailable;
      }

      bool setVolume(float vol)
      {
        auto const lock = std::scoped_lock{_handleMutex};
        float const clamped = std::clamp(vol, 0.0F, 1.0F);
        _softwareVolume = clamped;

        if (_volumeMode.load() == detail::AlsaVolumeControlMode::HardwareMixer && !applyHardwareVolume(clamped))
        {
          AUDIO_LOG_WARN("AlsaExclusiveBackend: Hardware volume write failed, falling back to software gain");
          _volumeMode = detail::AlsaVolumeControlMode::SoftwareGain;
          return false;
        }

        return true;
      }

      bool setMuted(bool mute)
      {
        auto const lock = std::scoped_lock{_handleMutex};
        _softwareMuted = mute;

        if (_volumeMode.load() == detail::AlsaVolumeControlMode::HardwareMixer && !applyHardwareMute(mute))
        {
          AUDIO_LOG_WARN("AlsaExclusiveBackend: Hardware mute write failed, falling back to software gain");
          _volumeMode = detail::AlsaVolumeControlMode::SoftwareGain;
          return false;
        }

        return true;
      }

      float readHardwareVolume() const
      {
        auto const lock = std::scoped_lock{_handleMutex};

        if (_mixerElem == nullptr)
        {
          return 1.0F;
        }

        if (_hasDB)
        {
          if (long db = 0L; ::snd_mixer_selem_get_playback_dB(_mixerElem, SND_MIXER_SCHN_MONO, &db) == 0)
          {
            return std::clamp(static_cast<float>(db - _dbMin) / static_cast<float>(_dbMax - _dbMin), 0.0F, 1.0F);
          }
        }

        if (long val = 0L; ::snd_mixer_selem_get_playback_volume(_mixerElem, SND_MIXER_SCHN_MONO, &val) == 0)
        {
          return std::clamp(static_cast<float>(val - _volMin) / static_cast<float>(_volMax - _volMin), 0.0F, 1.0F);
        }

        return 1.0F;
      }

      bool readHardwareMuted() const
      {
        auto const lock = std::scoped_lock{_handleMutex};

        if (_mixerElem == nullptr)
        {
          return false;
        }

        if (int val = 0; ::snd_mixer_selem_get_playback_switch(_mixerElem, SND_MIXER_SCHN_MONO, &val) == 0)
        {
          return val == 0;
        }

        return false;
      }

      std::string const& mixerElemName() const { return _mixerElemName; }
      detail::AlsaVolumeControlMode volumeMode() const { return _volumeMode.load(); }
      float softwareVolume() const { return _softwareVolume.load(); }
      bool softwareMuted() const { return _softwareMuted.load(); }

    private:
      bool openMixer(::snd_pcm_t* pcm)
      {
        ::snd_pcm_info_t* info = nullptr;
        snd_pcm_info_alloca(&info);

        if (::snd_pcm_info(pcm, info) < 0)
        {
          return false;
        }

        std::int32_t card = ::snd_pcm_info_get_card(info);
        ::snd_mixer_t* raw = nullptr;

        if (::snd_mixer_open(&raw, 0) < 0)
        {
          return false;
        }

        _mixerPtr.reset(raw);

        if (auto const cardStr = std::format("hw:{}", card); ::snd_mixer_attach(raw, cardStr.c_str()) < 0)
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

        return true;
      }

      bool tryUseMixerCandidate(AlsaMixerCandidate const& candidate)
      {
        auto const optRange = optPlaybackVolumeRange(candidate.elem);

        if (!optRange || !verifyMixerWriteReadback(candidate, *optRange))
        {
          return false;
        }

        long dbRangeMin = 0L;
        long dbRangeMax = 0L;

        _mixerElem = candidate.elem;
        _mixerElemName = candidate.name;
        _volMin = optRange->min;
        _volMax = optRange->max;
        _hasDB = (::snd_mixer_selem_get_playback_dB_range(_mixerElem, &dbRangeMin, &dbRangeMax) == 0 &&
                  dbRangeMax > dbRangeMin);
        _dbMin = static_cast<std::ptrdiff_t>(dbRangeMin);
        _dbMax = static_cast<std::ptrdiff_t>(dbRangeMax);
        _volumeMode = detail::AlsaVolumeControlMode::HardwareMixer;
        return true;
      }

      bool applyHardwareVolume(float vol) const
      {
        if (_mixerElem == nullptr)
        {
          return false;
        }

        float const clamped = std::clamp(vol, 0.0F, 1.0F);
        std::int32_t err = 0;

        if (_hasDB)
        {
          auto const db =
            toAlsaMixerLevel(_dbMin) + std::lround(static_cast<float>(_dbMax - _dbMin) * static_cast<double>(clamped));
          err = ::snd_mixer_selem_set_playback_dB_all(_mixerElem, db, 0);
        }
        else
        {
          auto const val = toAlsaMixerLevel(_volMin) +
                           std::lround(static_cast<float>(_volMax - _volMin) * static_cast<double>(clamped));
          err = ::snd_mixer_selem_set_playback_volume_all(_mixerElem, val);
        }

        return err == 0;
      }

      bool applyHardwareMute(bool mute) const
      {
        if (_mixerElem == nullptr)
        {
          return false;
        }

        return ::snd_mixer_selem_set_playback_switch_all(_mixerElem, mute ? 0 : 1) == 0;
      }

      struct AlsaMixerDeleter final
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

      // Serializes every snd_mixer_* handle call. The playback loop only reads
      // the atomics below (never the handle), so it never contends; this guards
      // the control-thread set/read/graph-publish paths against each other.
      mutable std::mutex _handleMutex;

      AlsaMixerPtr _mixerPtr;
      ::snd_mixer_elem_t* _mixerElem = nullptr; // non-owning
      std::string _mixerElemName;               // debug/log
      std::ptrdiff_t _volMin = 0;
      std::ptrdiff_t _volMax = 100;
      bool _hasDB = false;
      std::ptrdiff_t _dbMin = 0;
      std::ptrdiff_t _dbMax = 0;

      std::atomic<float> _softwareVolume{1.0F};
      std::atomic<bool> _softwareMuted{false};
      std::atomic<detail::AlsaVolumeControlMode> _volumeMode{detail::AlsaVolumeControlMode::Unavailable};
    };
  } // namespace

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
    Format format;
    IRenderTarget* renderTarget = nullptr;

    AlsaPcmPtr pcmPtr;
    std::jthread thread;
    std::atomic<bool> paused{false};
    mutable std::atomic<bool> fatalStreamError{false};
    bool canPause = false;

    AlsaMixerController mixer;
    ::snd_pcm_format_t alsaFormat = SND_PCM_FORMAT_S16_LE;
    bool is3Byte24Bit = false;

    detail::AlsaGraphRegistry* graphRegistry = nullptr;

    explicit Impl(std::string name, detail::AlsaGraphRegistry* graphRegistryArg)
      : deviceName{std::move(name)}, graphRegistry{graphRegistryArg}
    {
    }

    void playbackLoop(std::stop_token const& stopToken) const;
    void syncPauseState(bool& devicePaused) const;
    void recoverFromXrun(std::int32_t err) const;

    void publishGraphState() const;

    Result<> setVolumeProperty(PropertyValue const& value);
    Result<> setMutedProperty(PropertyValue const& value);

    Result<> configureHwParams(::snd_pcm_t* pcm,
                               Format& format,
                               ::snd_pcm_format_t& alsaFormat,
                               ::snd_pcm_uframes_t& periodSize);
    Result<> configureSwParams(::snd_pcm_t* pcm, ::snd_pcm_uframes_t periodSize);

    bool waitForFrames(::snd_pcm_uframes_t periodSize) const;
    void handleXrun(std::int32_t err) const;
    void commitFrames(::snd_pcm_uframes_t offset, ::snd_pcm_uframes_t framesRead) const;
  };

  void AlsaExclusiveBackend::Impl::playbackLoop(std::stop_token const& stopToken) const
  {
    ::snd_pcm_uframes_t periodSize = 0;
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);

    if (::snd_pcm_hw_params_current(pcmPtr.get(), params) == 0)
    {
      ::snd_pcm_hw_params_get_period_size(params, &periodSize, nullptr);
    }

    if (periodSize == 0)
    {
      periodSize = kDefaultPeriodSize;
    }

    std::size_t const bytesPerFrame = (static_cast<std::size_t>(format.bitDepth) / 8) * format.channels;

    // Tracks the device-side pause state owned exclusively by this thread.
    // pause()/resume() only flip the `paused` atomic; the edge is applied here.
    bool devicePaused = false;

    while (!stopToken.stop_requested() && !fatalStreamError.load(std::memory_order_relaxed))
    {
      syncPauseState(devicePaused);

      if (devicePaused)
      {
        std::this_thread::sleep_for(kPollRetryDelay);
        continue;
      }

      if (!waitForFrames(periodSize))
      {
        continue;
      }

      ::snd_pcm_uframes_t frames = periodSize;
      ::snd_pcm_uframes_t offset = 0;
      ::snd_pcm_channel_area_t const* areas = nullptr;

      if (auto const err = ::snd_pcm_mmap_begin(pcmPtr.get(), &areas, &offset, &frames); err < 0)
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

      std::size_t const bytesRead = renderTarget->readPcm({dst, bytesToRead});

      // Per the IRenderTarget contract readPcm returns whole frames; commit only
      // the whole-frame portion defensively. A partial frame must never be
      // committed (it would desync channel alignment) nor gain-scaled.
      auto const framesRead = static_cast<::snd_pcm_uframes_t>(bytesRead / bytesPerFrame);

      if (framesRead > 0)
      {
        auto const committedBytes = static_cast<std::size_t>(framesRead) * bytesPerFrame;

        if (mixer.volumeMode() == detail::AlsaVolumeControlMode::SoftwareGain)
        {
          detail::applyAlsaSoftwareGain({dst, committedBytes},
                                        format.bitDepth,
                                        format.validBits,
                                        is3Byte24Bit,
                                        mixer.softwareMuted() ? 0.0F : mixer.softwareVolume());
        }

        commitFrames(offset, framesRead);
      }
      else
      {
        ::snd_pcm_mmap_commit(pcmPtr.get(), offset, 0); // Release back to ALSA

        if (renderTarget->isSourceDrained())
        {
          ::snd_pcm_drain(pcmPtr.get());
          renderTarget->onDrainComplete();
          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
    }
  }

  void AlsaExclusiveBackend::Impl::syncPauseState(bool& devicePaused) const
  {
    // All snd_pcm_* state transitions are marshalled onto this loop thread
    // because snd_pcm_t is not thread-safe. Detect the pause/resume edge and
    // drive the device locally.
    if (bool const wantPaused = paused.load(std::memory_order_relaxed); wantPaused != devicePaused)
    {
      if (wantPaused)
      {
        if (canPause)
        {
          ::snd_pcm_pause(pcmPtr.get(), 1);
        }
        else
        {
          ::snd_pcm_drop(pcmPtr.get());
        }
      }
      else if (!canPause || ::snd_pcm_pause(pcmPtr.get(), 0) < 0)
      {
        ::snd_pcm_prepare(pcmPtr.get());
      }

      devicePaused = wantPaused;
    }
  }

  void AlsaExclusiveBackend::Impl::commitFrames(::snd_pcm_uframes_t offset, ::snd_pcm_uframes_t framesRead) const
  {
    auto const committed = ::snd_pcm_mmap_commit(pcmPtr.get(), offset, framesRead);

    if (committed < 0)
    {
      handleXrun(static_cast<std::int32_t>(committed));
    }
    else
    {
      // XRUN recovery via snd_pcm_prepare leaves the device in
      // PREPARED state. If the auto-start threshold has been met
      // by now, explicitly kick the device into RUNNING.
      if (auto const state = ::snd_pcm_state(pcmPtr.get()); state == SND_PCM_STATE_PREPARED)
      {
        ::snd_pcm_start(pcmPtr.get());
      }

      renderTarget->onPositionAdvanced(static_cast<std::uint32_t>(committed));
    }
  }

  bool AlsaExclusiveBackend::Impl::waitForFrames(::snd_pcm_uframes_t periodSize) const
  {
    auto const avail = ::snd_pcm_avail_update(pcmPtr.get());

    if (avail < 0)
    {
      handleXrun(static_cast<std::int32_t>(avail));
      return false;
    }

    if (std::cmp_less(avail, periodSize))
    {
      // Only block on the pollfd when the device is truly running.
      // In PREPARED / XRUN / SUSPENDED states the fd may never signal,
      // so we fall back to a short sleep to keep the loop responsive.
      if (::snd_pcm_state(pcmPtr.get()) == SND_PCM_STATE_RUNNING)
      {
        ::snd_pcm_wait(pcmPtr.get(), static_cast<std::int32_t>(kAlsaWaitTimeout.count()));
      }
      else
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }

      return false;
    }

    return true;
  }

  void AlsaExclusiveBackend::Impl::handleXrun(std::int32_t err) const
  {
    recoverFromXrun(err);
  }

  void AlsaExclusiveBackend::Impl::recoverFromXrun(std::int32_t err) const
  {
    if (err == -EPIPE)
    {
      renderTarget->onUnderrun();
      ::snd_pcm_prepare(pcmPtr.get());
    }
    else if (err == -ESTRPIPE)
    {
      while (::snd_pcm_resume(pcmPtr.get()) == -EAGAIN)
      {
        std::this_thread::sleep_for(kPollRetryDelay);
      }
    }
    else if (detail::isUnrecoverableAlsaPcmError(err))
    {
      auto const errorMsg = std::string{"ALSA: Unrecoverable stream state"};
      AUDIO_LOG_ERROR("{}", errorMsg);
      fatalStreamError.store(true, std::memory_order_relaxed);
      renderTarget->onBackendError(errorMsg);
    }
  }

  void AlsaExclusiveBackend::Impl::publishGraphState() const
  {
    if (graphRegistry == nullptr)
    {
      return;
    }

    auto const mode = mixer.volumeMode();
    float vol = 1.0F;
    bool muted = false;

    if (mode == detail::AlsaVolumeControlMode::HardwareMixer)
    {
      vol = mixer.readHardwareVolume();
      muted = mixer.readHardwareMuted();
    }
    else
    {
      vol = mixer.softwareVolume();
      muted = mixer.softwareMuted();
    }

    graphRegistry->publish({.routeAnchor = deviceName, .volume = vol, .muted = muted, .volumeMode = mode});
  }
  Result<> AlsaExclusiveBackend::Impl::setVolumeProperty(PropertyValue const& value)
  {
    mixer.setVolume(std::get<float>(value));
    publishGraphState();
    return {};
  }

  Result<> AlsaExclusiveBackend::Impl::setMutedProperty(PropertyValue const& value)
  {
    mixer.setMuted(std::get<bool>(value));
    publishGraphState();
    return {};
  }

  Result<> AlsaExclusiveBackend::Impl::configureHwParams(::snd_pcm_t* pcm,
                                                         Format& format,
                                                         ::snd_pcm_format_t& alsaFormat,
                                                         ::snd_pcm_uframes_t& periodSize)
  {
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params); // macro

    if (::snd_pcm_hw_params_any(pcm, params) < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to init ALSA hw params");
    }

    if (::snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0)
    {
      return makeError(Error::Code::FormatRejected, "No MMAP interleaved support");
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
          return makeError(Error::Code::FormatRejected, "Hardware supports neither S16_LE nor S32_LE");
        }
      }
      else
      {
        return makeError(Error::Code::FormatRejected, "Format not supported by hardware");
      }
    }

    std::uint32_t rate = format.sampleRate;

    if (::snd_pcm_hw_params_set_rate_near(pcm, params, &rate, nullptr) < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to set rate");
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
      return makeError(Error::Code::FormatRejected, "Failed to set channels");
    }

    std::uint32_t periods = 4;
    ::snd_pcm_hw_params_set_periods_near(pcm, params, &periods, nullptr);

    periodSize = kDefaultPeriodSize;
    ::snd_pcm_hw_params_set_period_size_near(pcm, params, &periodSize, nullptr);

    if (::snd_pcm_hw_params(pcm, params) < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to apply hw params");
    }

    canPause = (::snd_pcm_hw_params_can_pause(params) == 1);
    format.sampleRate = rate;
    return {};
  }

  Result<> AlsaExclusiveBackend::Impl::configureSwParams(::snd_pcm_t* pcm, ::snd_pcm_uframes_t periodSize)
  {
    ::snd_pcm_sw_params_t* swParams = nullptr;
    snd_pcm_sw_params_alloca(&swParams);

    if (::snd_pcm_sw_params_current(pcm, swParams) < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to get sw params");
    }

    ::snd_pcm_sw_params_set_start_threshold(pcm, swParams, periodSize);
    ::snd_pcm_sw_params_set_avail_min(pcm, swParams, periodSize);

    if (::snd_pcm_sw_params(pcm, swParams) < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to apply sw params");
    }

    return {};
  }

  AlsaExclusiveBackend::AlsaExclusiveBackend(Device const& device, ProfileId const& /*profile*/)
    : _implPtr{std::make_unique<Impl>(device.id.raw(), nullptr)}
  {
    AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Creating backend instance for device '{}'", _implPtr->deviceName);
  }

  AlsaExclusiveBackend::AlsaExclusiveBackend(Device const& device,
                                             ProfileId const& /*profile*/,
                                             detail::AlsaGraphRegistry& graphRegistry)
    : _implPtr{std::make_unique<Impl>(device.id.raw(), &graphRegistry)}
  {
    AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Creating backend instance for device '{}'", _implPtr->deviceName);
  }

  AlsaExclusiveBackend::~AlsaExclusiveBackend()
  {
    AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Destroying backend instance");
    stop();
    close();
  }

  Result<> AlsaExclusiveBackend::open(Format const& format, IRenderTarget* target)
  {
    _implPtr->format = format;
    _implPtr->renderTarget = target;
    AUDIO_LOG_INFO("AlsaExclusiveBackend: Opening device '{}' with format {}Hz/{}b/{}ch",
                   _implPtr->deviceName,
                   format.sampleRate,
                   static_cast<int>(format.bitDepth),
                   static_cast<int>(format.channels));

    close();
    _implPtr->fatalStreamError.store(false, std::memory_order_relaxed);

    ::snd_pcm_t* pcm = nullptr;

    if (::snd_pcm_open(&pcm, _implPtr->deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0)
    {
      return makeError(
        Error::Code::DeviceNotFound, std::format("Failed to open ALSA device: {}", _implPtr->deviceName));
    }

    auto safePcmPtr = Impl::AlsaPcmPtr{pcm};
    auto currentFormat = Format{format};
    auto alsaFormat = SND_PCM_FORMAT_S16_LE;
    ::snd_pcm_uframes_t periodSize = 0;

    if (auto const res = _implPtr->configureHwParams(safePcmPtr.get(), currentFormat, alsaFormat, periodSize); !res)
    {
      return res;
    }

    if (auto const res = _implPtr->configureSwParams(safePcmPtr.get(), periodSize); !res)
    {
      return res;
    }

    _implPtr->format = currentFormat;
    _implPtr->alsaFormat = alsaFormat;
    _implPtr->is3Byte24Bit = (alsaFormat == SND_PCM_FORMAT_S24_3LE);

    AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Device pause support: {}, Negotiated Rate: {}Hz",
                    _implPtr->canPause,
                    _implPtr->format.sampleRate);

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
      _implPtr->format.bitDepth = hwBitDepth;
      _implPtr->format.validBits = format.bitDepth;

      _implPtr->renderTarget->onFormatChanged(_implPtr->format);
    }

    _implPtr->renderTarget->onRouteReady(_implPtr->deviceName);

    _implPtr->pcmPtr = std::move(safePcmPtr);

    if (!_implPtr->mixer.init(_implPtr->pcmPtr.get()))
    {
      AUDIO_LOG_DEBUG("AlsaExclusiveBackend: Hardware mixer probe failed for device '{}', using software fallback",
                      _implPtr->deviceName);
    }

    _implPtr->publishGraphState();

    return {};
  }

  void AlsaExclusiveBackend::start()
  {
    if (!_implPtr->pcmPtr)
    {
      return;
    }

    if (!_implPtr->thread.joinable())
    {
      _implPtr->thread = std::jthread{[this](std::stop_token const& st)
                                      {
                                        setCurrentThreadName("AlsaPlayback");
                                        _implPtr->playbackLoop(st);
                                      }};
    }

    // The device is started from the playback loop thread (commitFrames kicks a
    // PREPARED device into RUNNING). snd_pcm_* is never touched here: the loop
    // owns the handle and snd_pcm_t is not thread-safe.
  }

  void AlsaExclusiveBackend::pause()
  {
    if (!_implPtr->pcmPtr)
    {
      return;
    }

    // Flip the intent only; the playback loop applies the device-side pause
    // (snd_pcm_pause / snd_pcm_drop) on its own thread to keep handle access
    // single-threaded.
    _implPtr->paused.store(true, std::memory_order_relaxed);
  }

  void AlsaExclusiveBackend::resume()
  {
    if (!_implPtr->pcmPtr)
    {
      return;
    }

    // Flip the intent only; the playback loop applies the device-side resume
    // (snd_pcm_pause release, or prepare + auto-start) on its own thread.
    _implPtr->paused.store(false, std::memory_order_relaxed);
  }

  void AlsaExclusiveBackend::flush()
  {
    // The playback loop owns snd_pcm_t while it is running. Reuse stop() as
    // the quiescent point before issuing flush-side state changes.
    stop();
  }

  void AlsaExclusiveBackend::stop()
  {
    _implPtr->thread.request_stop();

    if (_implPtr->thread.joinable() && std::this_thread::get_id() != _implPtr->thread.get_id())
    {
      _implPtr->thread.join();
    }

    if (_implPtr->pcmPtr)
    {
      ::snd_pcm_drop(_implPtr->pcmPtr.get());
      ::snd_pcm_prepare(_implPtr->pcmPtr.get());
    }
  }

  void AlsaExclusiveBackend::close()
  {
    if (_implPtr->graphRegistry != nullptr)
    {
      _implPtr->graphRegistry->clear(_implPtr->deviceName);
    }

    stop();
    _implPtr->pcmPtr.reset();
    _implPtr->mixer.close();
  }

  Result<> AlsaExclusiveBackend::setProperty(PropertyId id, PropertyValue const& value)
  {
    if (id == PropertyId::Volume)
    {
      return _implPtr->setVolumeProperty(value);
    }

    if (id == PropertyId::Muted)
    {
      return _implPtr->setMutedProperty(value);
    }

    return makeError(Error::Code::NotSupported);
  }

  Result<PropertyValue> AlsaExclusiveBackend::property(PropertyId id) const
  {
    if (id == PropertyId::Volume)
    {
      if (_implPtr->mixer.volumeMode() == detail::AlsaVolumeControlMode::HardwareMixer)
      {
        return _implPtr->mixer.readHardwareVolume();
      }

      return _implPtr->mixer.softwareVolume();
    }

    if (id == PropertyId::Muted)
    {
      if (_implPtr->mixer.volumeMode() == detail::AlsaVolumeControlMode::HardwareMixer)
      {
        return _implPtr->mixer.readHardwareMuted();
      }

      return _implPtr->mixer.softwareMuted();
    }

    return makeError(Error::Code::NotSupported);
  }

  PropertyInfo AlsaExclusiveBackend::queryProperty(PropertyId id) const noexcept
  {
    if (id == PropertyId::Volume)
    {
      bool const available =
        _implPtr != nullptr && _implPtr->mixer.volumeMode() != detail::AlsaVolumeControlMode::Unavailable;
      bool const hardware =
        _implPtr != nullptr && _implPtr->mixer.volumeMode() == detail::AlsaVolumeControlMode::HardwareMixer;
      return {.canRead = true,
              .canWrite = true,
              .isAvailable = available,
              .emitsChangeNotifications = false,
              .isHardwareAssisted = hardware};
    }

    if (id == PropertyId::Muted)
    {
      bool const available =
        _implPtr != nullptr && _implPtr->mixer.volumeMode() != detail::AlsaVolumeControlMode::Unavailable;
      return {.canRead = true,
              .canWrite = true,
              .isAvailable = available,
              .emitsChangeNotifications = false,
              .isHardwareAssisted = false};
    }

    return {};
  }

  BackendId AlsaExclusiveBackend::backendId() const noexcept
  {
    return kBackendAlsa;
  }

  ProfileId AlsaExclusiveBackend::profileId() const noexcept
  {
    return kProfileExclusive;
  }
} // namespace ao::audio::backend
