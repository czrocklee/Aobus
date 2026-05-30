// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/Property.h>
#include <ao/audio/backend/AlsaExclusiveBackend.h>
#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
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
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  constexpr int kAlsaWaitTimeoutMs = 500;
  constexpr int kPollRetryDelayMs = 10;
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
      auto value = 0L;

      if (::snd_mixer_selem_get_playback_volume(elem, channel, &value) < 0)
      {
        return std::nullopt;
      }

      return static_cast<std::ptrdiff_t>(value);
    }

    std::optional<AlsaMixerRange> optPlaybackVolumeRange(::snd_mixer_elem_t* elem)
    {
      auto min = 0L;
      auto max = 0L;

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

    AlsaMixerPtr mixerPtr;
    ::snd_mixer_elem_t* mixerElem = nullptr; // non-owning
    std::string mixerElemName;               // debug/log
    std::ptrdiff_t volMin = 0;
    std::ptrdiff_t volMax = 100;
    bool hasDB = false;
    std::ptrdiff_t dbMin = 0;
    std::ptrdiff_t dbMax = 0;

    std::atomic<float> softwareVolume{1.0F};
    std::atomic<bool> softwareMuted{false};
    std::atomic<detail::AlsaVolumeControlMode> volumeMode{detail::AlsaVolumeControlMode::Unavailable};
    ::snd_pcm_format_t alsaFormat = SND_PCM_FORMAT_S16_LE;
    bool is3Byte24Bit = false;

    detail::AlsaGraphRegistry* graphRegistryPtr = nullptr;

    explicit Impl(std::string name, detail::AlsaGraphRegistry* graphRegistry)
      : deviceName{std::move(name)}, graphRegistryPtr{graphRegistry}
    {
    }

    void playbackLoop(std::stop_token const& stopToken) const;
    void recoverFromXrun(std::int32_t err) const;

    bool initMixer(::snd_pcm_t* pcm);
    bool openMixer(::snd_pcm_t* pcm);
    bool tryUseMixerCandidate(AlsaMixerCandidate const& candidate);
    void publishGraphState() const;

    Result<> setVolumeProperty(PropertyValue const& value);
    Result<> setMutedProperty(PropertyValue const& value);
    bool applyHardwareVolume(float vol) const;
    bool applyHardwareMute(bool mute) const;
    float readHardwareVolume() const;

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

    while (!stopToken.stop_requested())
    {
      if (paused.load(std::memory_order_relaxed))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{kPollRetryDelayMs});
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

      if (bytesRead > 0)
      {
        if (volumeMode.load(std::memory_order_relaxed) == detail::AlsaVolumeControlMode::SoftwareGain)
        {
          detail::applyAlsaSoftwareGain({dst, bytesRead},
                                        format.bitDepth,
                                        format.validBits,
                                        is3Byte24Bit,
                                        softwareMuted ? 0.0F : softwareVolume.load());
        }

        auto const framesRead = static_cast<::snd_pcm_uframes_t>(bytesRead / (bytesPerFrame));
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
        ::snd_pcm_wait(pcmPtr.get(), kAlsaWaitTimeoutMs);
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
        std::this_thread::sleep_for(std::chrono::milliseconds{kPollRetryDelayMs});
      }
    }
    else if (err == -ENODEV || err == -EBADF)
    {
      auto const errorMsg = std::string{"ALSA: Unrecoverable stream state"};
      AUDIO_LOG_ERROR("{}", errorMsg);
      renderTarget->onBackendError(errorMsg);
    }
  }

  bool AlsaExclusiveBackend::Impl::initMixer(::snd_pcm_t* pcm)
  {
    if (!openMixer(pcm))
    {
      volumeMode = detail::AlsaVolumeControlMode::SoftwareGain;
      return false;
    }

    for (auto const& candidate : collectMixerCandidates(mixerPtr.get()))
    {
      if (tryUseMixerCandidate(candidate))
      {
        AUDIO_LOG_INFO("AlsaExclusiveBackend: Hardware mixer '{}' selected and verified", mixerElemName);
        return true;
      }
    }

    volumeMode = detail::AlsaVolumeControlMode::SoftwareGain;
    AUDIO_LOG_INFO("AlsaExclusiveBackend: No functional hardware mixer found, using software fallback");
    return false;
  }

  bool AlsaExclusiveBackend::Impl::openMixer(::snd_pcm_t* pcm)
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

    mixerPtr.reset(raw);

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

  bool AlsaExclusiveBackend::Impl::tryUseMixerCandidate(AlsaMixerCandidate const& candidate)
  {
    auto const optRange = optPlaybackVolumeRange(candidate.elem);

    if (!optRange || !verifyMixerWriteReadback(candidate, *optRange))
    {
      return false;
    }

    auto dbRangeMin = 0L;
    auto dbRangeMax = 0L;

    mixerElem = candidate.elem;
    mixerElemName = candidate.name;
    volMin = optRange->min;
    volMax = optRange->max;
    hasDB =
      (::snd_mixer_selem_get_playback_dB_range(mixerElem, &dbRangeMin, &dbRangeMax) == 0 && dbRangeMax > dbRangeMin);
    dbMin = static_cast<std::ptrdiff_t>(dbRangeMin);
    dbMax = static_cast<std::ptrdiff_t>(dbRangeMax);
    volumeMode = detail::AlsaVolumeControlMode::HardwareMixer;
    return true;
  }

  void AlsaExclusiveBackend::Impl::publishGraphState() const
  {
    if (graphRegistryPtr == nullptr)
    {
      return;
    }

    auto const mode = volumeMode.load();
    float vol = 1.0F;
    bool muted = false;

    if (mode == detail::AlsaVolumeControlMode::HardwareMixer)
    {
      vol = readHardwareVolume();

      if (int val = 0; ::snd_mixer_selem_get_playback_switch(mixerElem, SND_MIXER_SCHN_MONO, &val) == 0)
      {
        muted = (val == 0);
      }
    }
    else
    {
      vol = softwareVolume.load();
      muted = softwareMuted.load();
    }

    graphRegistryPtr->publish({.routeAnchor = deviceName, .volume = vol, .muted = muted, .volumeMode = mode});
  }

  Result<> AlsaExclusiveBackend::Impl::setVolumeProperty(PropertyValue const& value)
  {
    float const vol = std::clamp(std::get<float>(value), 0.0F, 1.0F);
    softwareVolume = vol;

    if (volumeMode.load() == detail::AlsaVolumeControlMode::HardwareMixer && !applyHardwareVolume(vol))
    {
      AUDIO_LOG_WARN("AlsaExclusiveBackend: Hardware volume write failed, falling back to software gain");
      volumeMode = detail::AlsaVolumeControlMode::SoftwareGain;
    }

    publishGraphState();
    return {};
  }

  Result<> AlsaExclusiveBackend::Impl::setMutedProperty(PropertyValue const& value)
  {
    bool const mute = std::get<bool>(value);
    softwareMuted = mute;

    if (volumeMode.load() == detail::AlsaVolumeControlMode::HardwareMixer && !applyHardwareMute(mute))
    {
      AUDIO_LOG_WARN("AlsaExclusiveBackend: Hardware mute write failed, falling back to software gain");
      volumeMode = detail::AlsaVolumeControlMode::SoftwareGain;
    }

    publishGraphState();
    return {};
  }

  bool AlsaExclusiveBackend::Impl::applyHardwareVolume(float vol) const
  {
    if (mixerElem == nullptr)
    {
      return false;
    }

    float const clamped = std::clamp(vol, 0.0F, 1.0F);
    std::int32_t err = 0;

    if (hasDB)
    {
      auto const db =
        toAlsaMixerLevel(dbMin) + std::lround(static_cast<float>(dbMax - dbMin) * static_cast<double>(clamped));
      err = ::snd_mixer_selem_set_playback_dB_all(mixerElem, db, 0);
    }
    else
    {
      auto const val =
        toAlsaMixerLevel(volMin) + std::lround(static_cast<float>(volMax - volMin) * static_cast<double>(clamped));
      err = ::snd_mixer_selem_set_playback_volume_all(mixerElem, val);
    }

    return err == 0;
  }

  bool AlsaExclusiveBackend::Impl::applyHardwareMute(bool mute) const
  {
    if (mixerElem == nullptr)
    {
      return false;
    }

    return ::snd_mixer_selem_set_playback_switch_all(mixerElem, mute ? 0 : 1) == 0;
  }

  float AlsaExclusiveBackend::Impl::readHardwareVolume() const
  {
    if (mixerElem == nullptr)
    {
      return 1.0F;
    }

    if (hasDB)
    {
      if (auto db = 0L; ::snd_mixer_selem_get_playback_dB(mixerElem, SND_MIXER_SCHN_MONO, &db) == 0)
      {
        return std::clamp(static_cast<float>(db - dbMin) / static_cast<float>(dbMax - dbMin), 0.0F, 1.0F);
      }
    }

    if (auto val = 0L; ::snd_mixer_selem_get_playback_volume(mixerElem, SND_MIXER_SCHN_MONO, &val) == 0)
    {
      return std::clamp(static_cast<float>(val - volMin) / static_cast<float>(volMax - volMin), 0.0F, 1.0F);
    }

    return 1.0F;
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

    if (!_implPtr->initMixer(_implPtr->pcmPtr.get()))
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

    ::snd_pcm_start(_implPtr->pcmPtr.get());
  }

  void AlsaExclusiveBackend::pause()
  {
    if (!_implPtr->pcmPtr)
    {
      return;
    }

    _implPtr->paused = true;

    if (_implPtr->canPause)
    {
      ::snd_pcm_pause(_implPtr->pcmPtr.get(), 1);
    }
    else
    {
      ::snd_pcm_drop(_implPtr->pcmPtr.get());
    }
  }

  void AlsaExclusiveBackend::resume()
  {
    if (!_implPtr->pcmPtr)
    {
      return;
    }

    _implPtr->paused = false;
    std::int32_t err = 0;

    if (_implPtr->canPause)
    {
      err = ::snd_pcm_pause(_implPtr->pcmPtr.get(), 0);
    }

    if (!_implPtr->canPause || err < 0)
    {
      ::snd_pcm_prepare(_implPtr->pcmPtr.get());
      ::snd_pcm_start(_implPtr->pcmPtr.get());
    }
  }

  void AlsaExclusiveBackend::flush()
  {
    if (_implPtr->pcmPtr)
    {
      ::snd_pcm_drop(_implPtr->pcmPtr.get());
      ::snd_pcm_prepare(_implPtr->pcmPtr.get());
    }
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
    if (_implPtr->graphRegistryPtr != nullptr)
    {
      _implPtr->graphRegistryPtr->clear(_implPtr->deviceName);
    }

    stop();
    _implPtr->pcmPtr.reset();
    _implPtr->mixerPtr.reset();
    _implPtr->mixerElem = nullptr;
    _implPtr->volumeMode = detail::AlsaVolumeControlMode::Unavailable;
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

    return std::unexpected(Error{.code = Error::Code::NotSupported});
  }

  Result<PropertyValue> AlsaExclusiveBackend::property(PropertyId id) const
  {
    if (id == PropertyId::Volume)
    {
      if (_implPtr->volumeMode.load() == detail::AlsaVolumeControlMode::HardwareMixer)
      {
        return _implPtr->readHardwareVolume();
      }

      return _implPtr->softwareVolume.load();
    }

    if (id == PropertyId::Muted)
    {
      if (_implPtr->volumeMode.load() == detail::AlsaVolumeControlMode::HardwareMixer)
      {
        if (int val = 0; ::snd_mixer_selem_get_playback_switch(_implPtr->mixerElem, SND_MIXER_SCHN_MONO, &val) == 0)
        {
          return val == 0;
        }
      }

      return _implPtr->softwareMuted.load();
    }

    return std::unexpected(Error{.code = Error::Code::NotSupported});
  }

  PropertyInfo AlsaExclusiveBackend::queryProperty(PropertyId id) const noexcept
  {
    if (id == PropertyId::Volume)
    {
      bool const available =
        _implPtr != nullptr && _implPtr->volumeMode.load() != detail::AlsaVolumeControlMode::Unavailable;
      bool const hardware =
        _implPtr != nullptr && _implPtr->volumeMode.load() == detail::AlsaVolumeControlMode::HardwareMixer;
      return {.canRead = true,
              .canWrite = true,
              .isAvailable = available,
              .emitsChangeNotifications = false,
              .isHardwareAssisted = hardware};
    }

    if (id == PropertyId::Muted)
    {
      bool const available =
        _implPtr != nullptr && _implPtr->volumeMode.load() != detail::AlsaVolumeControlMode::Unavailable;
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
