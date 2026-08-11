// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/AlsaExclusiveBackend.h>

#include "backend/detail/AlsaModeSelector.h"
#include "backend/detail/AlsaPcmFormat.h"
#include "backend/detail/AlsaPrewarmCache.h"
#include "detail/DecoderOutput.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/backend/detail/AlsaGraphRegistry.h>
#include <ao/audio/backend/detail/AlsaPcmError.h>
#include <ao/audio/backend/detail/AlsaPcmVolume.h>
#include <ao/audio/backend/detail/AudioBackendRenderProgress.h>
#include <ao/utility/ThreadName.h>

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
#include <exception>
#include <expected>
#include <format>
#include <limits>
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

    std::optional<PcmFormat> readCurrentHwFormat(::snd_pcm_t* const pcm)
    {
      ::snd_pcm_hw_params_t* params = nullptr;
      snd_pcm_hw_params_alloca(&params);

      if (::snd_pcm_hw_params_current(pcm, params) < 0)
      {
        return std::nullopt;
      }

      auto alsaFormat = SND_PCM_FORMAT_UNKNOWN;

      if (::snd_pcm_hw_params_get_format(params, &alsaFormat) < 0)
      {
        return std::nullopt;
      }

      auto optEncoding = detail::sampleEncodingFromAlsaFormat(alsaFormat);

      if (!optEncoding)
      {
        return std::nullopt;
      }

      unsigned int rate = 0;
      unsigned int channels = 0;

      if (::snd_pcm_hw_params_get_rate(params, &rate, nullptr) < 0 ||
          ::snd_pcm_hw_params_get_channels(params, &channels) < 0 ||
          channels > std::numeric_limits<std::uint8_t>::max())
      {
        return std::nullopt;
      }

      return PcmFormat{.sampleRate = rate, .channels = static_cast<std::uint8_t>(channels), .encoding = *optEncoding};
    }

    bool isConfiguredFormat(PcmFormat const& format) noexcept
    {
      return format.sampleRate != 0U && format.channels != 0U && format.encoding != SampleEncoding::Unknown;
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
        return false;
      }

      return true;
    }

    class [[nodiscard]] AlsaMixerSession final
    {
    public:
      AlsaMixerSession() = default;

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
            return true;
          }
        }

        _volumeMode = detail::AlsaVolumeControlMode::SoftwareGain;
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

        if (_hasDecibelRange)
        {
          if (long decibels = 0L; ::snd_mixer_selem_get_playback_dB(_mixerElem, SND_MIXER_SCHN_MONO, &decibels) == 0)
          {
            return std::clamp(
              static_cast<float>(decibels - _decibelMin) / static_cast<float>(_decibelMax - _decibelMin), 0.0F, 1.0F);
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
      bool isSoftwareMuted() const { return _softwareMuted.load(); }

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

        long decibelRangeMin = 0L;
        long decibelRangeMax = 0L;

        _mixerElem = candidate.elem;
        _mixerElemName = candidate.name;
        _volMin = optRange->min;
        _volMax = optRange->max;
        _hasDecibelRange =
          (::snd_mixer_selem_get_playback_dB_range(_mixerElem, &decibelRangeMin, &decibelRangeMax) == 0 &&
           decibelRangeMax > decibelRangeMin);
        _decibelMin = static_cast<std::ptrdiff_t>(decibelRangeMin);
        _decibelMax = static_cast<std::ptrdiff_t>(decibelRangeMax);
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

        if (_hasDecibelRange)
        {
          auto const decibelLevel =
            toAlsaMixerLevel(_decibelMin) +
            std::lround(static_cast<float>(_decibelMax - _decibelMin) * static_cast<double>(clamped));
          err = ::snd_mixer_selem_set_playback_dB_all(_mixerElem, decibelLevel, 0);
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
      bool _hasDecibelRange = false;
      std::ptrdiff_t _decibelMin = 0;
      std::ptrdiff_t _decibelMax = 0;

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

    // Single authority for the configured mode. Engine's open() result, the
    // playback loop, the prewarm cache, and both graph nodes read this one
    // value, so the endpoint Engine validated cannot diverge from the endpoint
    // the quality panel shows.
    std::optional<OpenedPcmMode> optOpenedMode;

    detail::AlsaPrewarmCache prewarmCache;
    RenderTarget* renderTarget = nullptr;

    AlsaPcmPtr pcmPtr;
    std::jthread thread;
    std::atomic<bool> paused{false};
    mutable std::atomic<bool> fatalStreamError{false};
    bool canPause = false;

    AlsaMixerSession mixer;

    detail::AlsaGraphRegistry* graphRegistry = nullptr;

    explicit Impl(std::string name, detail::AlsaGraphRegistry* graphRegistryHandle)
      : deviceName{std::move(name)}, graphRegistry{graphRegistryHandle}
    {
    }

    void playbackLoop(std::stop_token const& stopToken) const;
    void syncPauseState(bool& devicePaused) const;
    void recoverFromXrun(std::int32_t err) const;

    void publishGraphState() const;

    Result<> setVolumeProperty(PropertyValue const& value);
    Result<> setMutedProperty(PropertyValue const& value);

    struct NegotiatedMode final
    {
      OpenedPcmMode mode;
      ::snd_pcm_uframes_t periodSize = 0;
    };

    Result<NegotiatedMode> negotiateHwParams(::snd_pcm_t* pcm, SignalFormat const& sourceFormat);
    Result<> configureSwParams(::snd_pcm_t* pcm, ::snd_pcm_uframes_t periodSize);

    bool waitForFrames(::snd_pcm_uframes_t periodSize) const;
    void handleXrun(std::int32_t err) const;
    void commitFrames(::snd_pcm_uframes_t offset,
                      ::snd_pcm_uframes_t framesRead,
                      RenderPcmResult const& renderRes,
                      ::snd_pcm_uframes_t bufferSize,
                      ::snd_pcm_uframes_t startThreshold) const;
  };

  void AlsaExclusiveBackend::Impl::playbackLoop(std::stop_token const& stopToken) const
  {
    ::snd_pcm_uframes_t periodSize = 0;
    ::snd_pcm_uframes_t bufferSize = 0;
    ::snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);

    if (::snd_pcm_hw_params_current(pcmPtr.get(), params) == 0)
    {
      ::snd_pcm_hw_params_get_period_size(params, &periodSize, nullptr);
      ::snd_pcm_hw_params_get_buffer_size(params, &bufferSize);
    }

    if (periodSize == 0)
    {
      periodSize = kDefaultPeriodSize;
    }

    bufferSize = std::max(bufferSize, periodSize);

    AO_INVARIANT(optOpenedMode);
    auto const clientFormat = optOpenedMode->clientFormat;
    std::size_t const bytesPerFrame = frameBytes(clientFormat);

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
      auto* const dst = static_cast<std::byte*>(areas[0].addr) + ((areas[0].first + (offset * areas[0].step)) / 8);
      auto const bytesToRead = static_cast<std::size_t>(frames) * bytesPerFrame;

      auto const renderRes = renderTarget->renderPcm({dst, bytesToRead});
      std::size_t const bytesRead = renderRes.bytesWritten;

      // Per the RenderTarget contract renderPcm returns whole frames; commit only
      // the whole-frame portion defensively. A partial frame must never be
      // committed (it would desync channel alignment) nor gain-scaled.
      auto const framesRead = static_cast<::snd_pcm_uframes_t>(bytesRead / bytesPerFrame);

      if (framesRead > 0)
      {
        auto const committedBytes = static_cast<std::size_t>(framesRead) * bytesPerFrame;

        if (mixer.volumeMode() == detail::AlsaVolumeControlMode::SoftwareGain)
        {
          detail::applyAlsaSoftwareGain(
            {dst, committedBytes}, clientFormat.encoding, mixer.isSoftwareMuted() ? 0.0F : mixer.softwareVolume());
        }

        commitFrames(offset, framesRead, renderRes, bufferSize, periodSize);
      }
      else
      {
        ::snd_pcm_mmap_commit(pcmPtr.get(), offset, 0); // Release back to ALSA

        if (renderRes.drained)
        {
          ::snd_pcm_drain(pcmPtr.get());
          renderTarget->handleDrainComplete();
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

  void AlsaExclusiveBackend::Impl::commitFrames(::snd_pcm_uframes_t offset,
                                                ::snd_pcm_uframes_t framesRead,
                                                RenderPcmResult const& renderRes,
                                                ::snd_pcm_uframes_t bufferSize,
                                                ::snd_pcm_uframes_t startThreshold) const
  {
    auto const committed = ::snd_pcm_mmap_commit(pcmPtr.get(), offset, framesRead);

    if (committed < 0)
    {
      handleXrun(static_cast<std::int32_t>(committed));
    }
    else
    {
      if (::snd_pcm_state(pcmPtr.get()) == SND_PCM_STATE_PREPARED)
      {
        if (auto const available = ::snd_pcm_avail_update(pcmPtr.get()); available < 0)
        {
          handleXrun(static_cast<std::int32_t>(available));
        }
        else
        {
          auto const availableFrames = static_cast<::snd_pcm_uframes_t>(available);
          auto const queuedFrames = availableFrames < bufferSize ? bufferSize - availableFrames : 0;

          if (queuedFrames >= startThreshold)
          {
            if (auto const startStatus = ::snd_pcm_start(pcmPtr.get()); startStatus < 0)
            {
              handleXrun(startStatus);
            }
          }
        }
      }

      auto const committedPositionFrames = detail::committedPositionFrames(
        static_cast<std::uint64_t>(committed), renderRes.positionFrameOffset, renderRes.positionFrames);
      renderTarget->handlePositionAdvanced(static_cast<std::uint32_t>(committedPositionFrames));
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
      renderTarget->handleUnderrun();
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
      fatalStreamError.store(true, std::memory_order_relaxed);
      renderTarget->handleBackendError(errorMsg);
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
      muted = mixer.isSoftwareMuted();
    }

    auto optMode = std::optional<OpenedPcmMode>{};

    if (pcmPtr && optOpenedMode && isConfiguredFormat(optOpenedMode->clientFormat))
    {
      optMode = optOpenedMode;
    }

    graphRegistry->publish(
      {.routeAnchor = deviceName, .optMode = optMode, .volume = vol, .muted = muted, .volumeMode = mode});
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

  Result<AlsaExclusiveBackend::Impl::NegotiatedMode> AlsaExclusiveBackend::Impl::negotiateHwParams(
    ::snd_pcm_t* pcm,
    SignalFormat const& sourceFormat)
  {
    ::snd_pcm_hw_params_t* base = nullptr;
    snd_pcm_hw_params_alloca(&base); // macro

    if (::snd_pcm_hw_params_any(pcm, base) < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to init ALSA hw params");
    }

    if (::snd_pcm_hw_params_set_access(pcm, base, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0)
    {
      return makeError(Error::Code::FormatRejected, "No MMAP interleaved support");
    }

    // Exact rate, never rate_near: the admissible format set is a joint
    // constraint, so the mask below is only meaningful once the rate this track
    // actually needs is pinned. It also fails here, with a rate diagnosis,
    // instead of surfacing later as a misleading format rejection.
    if (::snd_pcm_hw_params_set_rate(pcm, base, sourceFormat.sampleRate, 0) < 0)
    {
      return makeError(
        Error::Code::FormatRejected,
        std::format(
          "ALSA device '{}' does not support {} Hz and Aobus has no resampler", deviceName, sourceFormat.sampleRate));
    }

    if (::snd_pcm_hw_params_set_channels(pcm, base, sourceFormat.channels) < 0)
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("ALSA device '{}' does not support {} channels and Aobus has no channel remapper",
                                   deviceName,
                                   static_cast<std::uint32_t>(sourceFormat.channels)));
    }

    ::snd_pcm_format_mask_t* mask = nullptr;
    snd_pcm_format_mask_alloca(&mask); // macro
    ::snd_pcm_hw_params_get_format_mask(base, mask);

    // Snapshot what this handle still admits at this rate and channel count.
    // Constraining a params copy is what makes the driver report significant
    // bits, so a 32-bit container in front of a 24-bit converter is visible
    // before anything is committed.
    auto evidence = std::vector<detail::AlsaModeEvidence>{};

    // One reusable scratch block: snd_pcm_hw_params_alloca lives until this
    // function returns, so allocating per iteration would grow the stack.
    ::snd_pcm_hw_params_t* trial = nullptr;
    snd_pcm_hw_params_alloca(&trial); // macro

    for (auto const encoding : detail::kAlsaCandidateEncodings)
    {
      auto const optAlsaFormat = detail::alsaFormatFromSampleEncoding(encoding);

      if (!optAlsaFormat || ::snd_pcm_format_mask_test(mask, *optAlsaFormat) == 0)
      {
        continue;
      }

      ::snd_pcm_hw_params_copy(trial, base);

      if (::snd_pcm_hw_params_set_format(pcm, trial, *optAlsaFormat) < 0)
      {
        continue;
      }

      auto const sbits = ::snd_pcm_hw_params_get_sbits(trial);
      evidence.push_back({.encoding = encoding,
                          .optSignificantBits = sbits > 0
                                                  ? std::optional{static_cast<std::uint8_t>(std::min(
                                                      sbits, static_cast<std::int32_t>(encodingNominalBits(encoding))))}
                                                  : std::nullopt});
    }

    auto const selectedRes = detail::selectAlsaMode(sourceFormat, evidence);

    if (!selectedRes)
    {
      return std::unexpected{selectedRes.error()};
    }

    auto const clientFormat = pcmFormat(sourceFormat, selectedRes->encoding);
    auto const optAlsaFormat = detail::alsaFormatFromSampleEncoding(selectedRes->encoding);
    AO_INVARIANT(optAlsaFormat);

    ::snd_pcm_hw_params_t* applied = nullptr;
    snd_pcm_hw_params_alloca(&applied); // macro
    ::snd_pcm_hw_params_copy(applied, base);

    if (::snd_pcm_hw_params_set_format(pcm, applied, *optAlsaFormat) < 0)
    {
      return makeError(Error::Code::InitFailed,
                       std::format("ALSA rejected {} after admitting it", sampleEncodingName(selectedRes->encoding)));
    }

    std::uint32_t periods = 4;
    ::snd_pcm_hw_params_set_periods_near(pcm, applied, &periods, nullptr);

    auto periodSize = static_cast<::snd_pcm_uframes_t>(kDefaultPeriodSize);
    ::snd_pcm_hw_params_set_period_size_near(pcm, applied, &periodSize, nullptr);

    if (::snd_pcm_hw_params(pcm, applied) < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to apply hw params");
    }

    auto const optCurrentFormat = readCurrentHwFormat(pcm);

    if (!optCurrentFormat)
    {
      return makeError(Error::Code::InitFailed, "Failed to read ALSA current hw params");
    }

    canPause = (::snd_pcm_hw_params_can_pause(applied) == 1);

    if (!samePcmMode(clientFormat, *optCurrentFormat))
    {
      return makeError(Error::Code::FormatRejected, "ALSA applied a different PCM mode than requested");
    }

    // Take the endpoint width from the configuration that was actually
    // applied. The selector read the same value from a trial copy, but only
    // this one describes the stream that is about to run.
    auto const appliedSbits = ::snd_pcm_hw_params_get_sbits(applied);

    if (appliedSbits <= 0)
    {
      return makeError(Error::Code::FormatRejected, "ALSA could not confirm significant bits for the applied mode");
    }

    auto const endpointBits = static_cast<std::uint8_t>(
      std::min(appliedSbits, static_cast<std::int32_t>(encodingNominalBits(selectedRes->encoding))));

    if (endpointBits < sourceFormat.precisionBits)
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("ALSA endpoint resolves {} bits but the track requires {} bits",
                                   static_cast<std::uint32_t>(endpointBits),
                                   static_cast<std::uint32_t>(sourceFormat.precisionBits)));
    }

    auto const endpointSignal = SignalFormat{
      .sampleRate = clientFormat.sampleRate,
      .channels = clientFormat.channels,
      .precisionBits = endpointBits,
      .sampleKind = isFloatEncoding(selectedRes->encoding) ? SampleKind::FloatingPoint : SampleKind::Integer};

    return NegotiatedMode{.mode = OpenedPcmMode{.clientFormat = clientFormat,
                                                .optEndpoint = ConfirmedEndpoint{.signalFormat = endpointSignal}},
                          .periodSize = periodSize};
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
  }

  AlsaExclusiveBackend::AlsaExclusiveBackend(Device const& device,
                                             ProfileId const& /*profile*/,
                                             detail::AlsaGraphRegistry& graphRegistry)
    : _implPtr{std::make_unique<Impl>(device.id.raw(), &graphRegistry)}
  {
  }

  AlsaExclusiveBackend::~AlsaExclusiveBackend()
  {
    stop();
    close();
  }

  std::optional<PcmFormat> AlsaExclusiveBackend::prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept
  {
    if (auto optCached = _implPtr->prewarmCache.find(sourceFormat); optCached)
    {
      return optCached;
    }

    return Backend::prewarmFormatHint(sourceFormat);
  }

  Result<OpenedPcmMode> AlsaExclusiveBackend::open(SignalFormat const& sourceFormat, RenderTarget& target)
  {
    close();

    // Reject a signal this backend could never carry before taking a device
    // away from whoever currently holds it.
    if (::ao::audio::detail::losslessPcmEncodings(sourceFormat).empty())
    {
      return makeError(Error::Code::NotSupported, "No lossless PCM encoding is available for ALSA");
    }

    _implPtr->fatalStreamError.store(false, std::memory_order_relaxed);

    ::snd_pcm_t* pcm = nullptr;

    auto const openStatus = ::snd_pcm_open(&pcm, _implPtr->deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0);

    if (openStatus < 0)
    {
      return makeError(
        detail::alsaPcmOpenErrorCode(openStatus),
        std::format("Failed to open ALSA device '{}': {}", _implPtr->deviceName, ::snd_strerror(openStatus)));
    }

    auto safePcmPtr = Impl::AlsaPcmPtr{pcm};

    if (::snd_pcm_type(safePcmPtr.get()) != SND_PCM_TYPE_HW)
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("ALSA exclusive output requires a direct hardware PCM: {}", _implPtr->deviceName));
    }

    auto negotiatedRes = _implPtr->negotiateHwParams(safePcmPtr.get(), sourceFormat);

    if (!negotiatedRes)
    {
      return std::unexpected{negotiatedRes.error()};
    }

    if (auto const resRes = _implPtr->configureSwParams(safePcmPtr.get(), negotiatedRes->periodSize); !resRes)
    {
      return std::unexpected{resRes.error()};
    }

    _implPtr->optOpenedMode = negotiatedRes->mode;
    _implPtr->prewarmCache.store(sourceFormat, negotiatedRes->mode.clientFormat);
    _implPtr->renderTarget = &target;

    _implPtr->pcmPtr = std::move(safePcmPtr);

    _implPtr->mixer.init(_implPtr->pcmPtr.get());

    _implPtr->publishGraphState();

    target.handleRouteReady(_implPtr->deviceName);

    return negotiatedRes->mode;
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

                                        try
                                        {
                                          _implPtr->playbackLoop(st);
                                        }
                                        catch (...)
                                        {
                                          AO_FATAL_EXCEPTION(std::current_exception(), "ALSA playback thread");
                                        }
                                      }};
    }

    // The playback loop fills the mmap buffer to the configured start
    // threshold and starts the device there. snd_pcm_* is never touched here:
    // the loop owns the handle and snd_pcm_t is not thread-safe.
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
    _implPtr->renderTarget = nullptr;

    // Cleared only after stop() joined the playback loop that reads it. The
    // prewarm cache deliberately survives: it describes the device, not this
    // stream, and dies with the backend when the selected device changes.
    _implPtr->optOpenedMode.reset();
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

      return _implPtr->mixer.isSoftwareMuted();
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

  BackendId AlsaExclusiveBackend::backendId() const
  {
    return kBackendAlsa;
  }

  ProfileId AlsaExclusiveBackend::profileId() const
  {
    return kProfileExclusive;
  }
} // namespace ao::audio::backend
