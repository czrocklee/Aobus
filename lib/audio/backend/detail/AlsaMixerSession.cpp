// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/AlsaMixerSession.h"

#include "backend/detail/AlsaGraphRegistry.h"
#include <ao/Error.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail
{
  namespace
  {
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

    using UnsignedMixerLevel = std::make_unsigned_t<AlsaMixerLevel>;

    UnsignedMixerLevel levelDistance(AlsaMixerLevel low, AlsaMixerLevel high) noexcept
    {
      // Unsigned differences retain small spans near long limits without signed overflow.
      return static_cast<UnsignedMixerLevel>(high) - static_cast<UnsignedMixerLevel>(low);
    }

    AlsaMixerLevel quantizedLevel(AlsaMixerLevelRange const& range, float normalized) noexcept
    {
      if (normalized <= 0.0F)
      {
        return range.min;
      }

      if (normalized >= 1.0F)
      {
        return range.max;
      }

      double const span = static_cast<double>(levelDistance(range.min, range.max));
      double const roundedOffset = std::round(span * static_cast<double>(normalized));

      if (roundedOffset <= 0.0)
      {
        return range.min;
      }

      if (roundedOffset >= span)
      {
        return range.max;
      }

      // Rebuild in the unsigned domain; C++ integer conversion preserves the signed result modulo its width.
      return static_cast<AlsaMixerLevel>(static_cast<UnsignedMixerLevel>(range.min) +
                                         static_cast<UnsignedMixerLevel>(roundedOffset));
    }

    std::optional<AlsaMixerLevelRange> usableDecibelRange(AlsaMixerElementSnapshot const& snapshot) noexcept
    {
      if (snapshot.optDecibelRange && snapshot.optDecibelRange->min != SND_CTL_TLV_DB_GAIN_MUTE)
      {
        return snapshot.optDecibelRange;
      }

      return std::nullopt;
    }

    float normalizedLevel(AlsaMixerLevelRange const& range, AlsaMixerLevel level) noexcept
    {
      if (level <= range.min)
      {
        return 0.0F;
      }

      if (level >= range.max)
      {
        return 1.0F;
      }

      double const span = static_cast<double>(levelDistance(range.min, range.max));
      double const normalized = static_cast<double>(levelDistance(range.min, level)) / span;
      return static_cast<float>(std::clamp(normalized, 0.0, 1.0));
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

    class NativeAlsaMixerIo final : public AlsaMixerIo
    {
    public:
      explicit NativeAlsaMixerIo(AlsaMixerPtr mixerPtr)
        : _mixerPtr{std::move(mixerPtr)}
      {
      }

      bool tryRefresh() override { return ::snd_mixer_handle_events(_mixerPtr.get()) >= 0; }

      std::vector<AlsaMixerElementId> elementIds() const override
      {
        auto ids = std::vector<AlsaMixerElementId>{};

        for (auto* elem = ::snd_mixer_first_elem(_mixerPtr.get()); elem != nullptr; elem = ::snd_mixer_elem_next(elem))
        {
          if (char const* const name = ::snd_mixer_selem_get_name(elem); name != nullptr)
          {
            ids.push_back({.name = name, .index = ::snd_mixer_selem_get_index(elem)});
          }
        }

        return ids;
      }

      std::optional<AlsaMixerElementSnapshot> inspect(AlsaMixerElementId const& id) const override
      {
        auto* const elem = findElement(id);

        if (elem == nullptr || ::snd_mixer_selem_is_active(elem) == 0 ||
            ::snd_mixer_selem_has_playback_volume(elem) == 0)
        {
          return std::nullopt;
        }

        long rawMin = 0L;
        long rawMax = 0L;

        if (::snd_mixer_selem_get_playback_volume_range(elem, &rawMin, &rawMax) < 0 || rawMax <= rawMin)
        {
          return std::nullopt;
        }

        auto rawLevels = std::vector<long>{};
        auto channels = std::vector<::snd_mixer_selem_channel_id_t>{};

        for (std::int32_t channelValue = 0; channelValue <= SND_MIXER_SCHN_LAST; ++channelValue)
        {
          auto const channel = static_cast<::snd_mixer_selem_channel_id_t>(channelValue);

          if (::snd_mixer_selem_has_playback_channel(elem, channel) == 0)
          {
            continue;
          }

          long rawLevel = 0L;

          if (::snd_mixer_selem_get_playback_volume(elem, channel, &rawLevel) < 0 || rawLevel < rawMin ||
              rawLevel > rawMax)
          {
            return std::nullopt;
          }

          channels.push_back(channel);
          rawLevels.push_back(rawLevel);
        }

        if (rawLevels.empty())
        {
          return std::nullopt;
        }

        auto optDecibelRange = std::optional<AlsaMixerLevelRange>{};
        auto decibelLevels = std::vector<long>{};

        if (auto decibelRange = AlsaMixerLevelRange{};
            ::snd_mixer_selem_get_playback_dB_range(elem, &decibelRange.min, &decibelRange.max) == 0 &&
            decibelRange.max > decibelRange.min)
        {
          bool readable = true;

          for (auto const channel : channels)
          {
            long decibelLevel = 0L;

            if (::snd_mixer_selem_get_playback_dB(elem, channel, &decibelLevel) < 0)
            {
              readable = false;
              break;
            }

            decibelLevels.push_back(decibelLevel);
          }

          if (readable)
          {
            optDecibelRange = decibelRange;
          }
          else
          {
            decibelLevels.clear();
          }
        }

        auto optHardwareMuted = std::optional<bool>{};

        if (::snd_mixer_selem_has_playback_switch(elem) != 0)
        {
          bool anyMuted = false;

          for (auto const channel : channels)
          {
            int enabled = 0;

            // A volume setter may commit the simple element's cached switch
            // controls too. Reject an element whose switch cache cannot be
            // refreshed completely rather than risk changing external mute.
            if (::snd_mixer_selem_get_playback_switch(elem, channel, &enabled) < 0)
            {
              return std::nullopt;
            }

            anyMuted = anyMuted || enabled == 0;
          }

          optHardwareMuted = anyMuted;
        }

        return AlsaMixerElementSnapshot{.id = id,
                                        .rawRange = {.min = rawMin, .max = rawMax},
                                        .rawLevels = std::move(rawLevels),
                                        .optDecibelRange = optDecibelRange,
                                        .decibelLevels = std::move(decibelLevels),
                                        .optHardwareMuted = optHardwareMuted};
      }

      bool trySetRawVolume(AlsaMixerElementId const& id, long level) override
      {
        auto* const elem = findElement(id);
        return elem != nullptr && ::snd_mixer_selem_set_playback_volume_all(elem, level) == 0;
      }

      bool trySetDecibelVolume(AlsaMixerElementId const& id, long level) override
      {
        auto* const elem = findElement(id);
        return elem != nullptr && ::snd_mixer_selem_set_playback_dB_all(elem, level, 0) == 0;
      }

    private:
      ::snd_mixer_elem_t* findElement(AlsaMixerElementId const& id) const
      {
        ::snd_mixer_selem_id_t* nativeId = nullptr;
        snd_mixer_selem_id_alloca(&nativeId);
        ::snd_mixer_selem_id_set_name(nativeId, id.name.c_str());
        ::snd_mixer_selem_id_set_index(nativeId, id.index);
        return ::snd_mixer_find_selem(_mixerPtr.get(), nativeId);
      }

      AlsaMixerPtr _mixerPtr;
    };

    class NativeAlsaMixerOpenFactory final : public AlsaMixerOpenFactory
    {
    public:
      std::unique_ptr<AlsaMixerIo> open(::snd_pcm_t* pcm) override
      {
        ::snd_pcm_info_t* info = nullptr;
        snd_pcm_info_alloca(&info);

        if (::snd_pcm_info(pcm, info) < 0)
        {
          return nullptr;
        }

        std::int32_t const card = ::snd_pcm_info_get_card(info);
        ::snd_mixer_t* mixer = nullptr;

        if (::snd_mixer_open(&mixer, 0) < 0)
        {
          return nullptr;
        }

        auto mixerPtr = AlsaMixerPtr{mixer};

        if (auto const cardName = std::format("hw:{}", card); ::snd_mixer_attach(mixer, cardName.c_str()) < 0 ||
                                                              ::snd_mixer_selem_register(mixer, nullptr, nullptr) < 0 ||
                                                              ::snd_mixer_load(mixer) < 0)
        {
          return nullptr;
        }

        return std::make_unique<NativeAlsaMixerIo>(std::move(mixerPtr));
      }
    };
  } // namespace

  AlsaMixerOpenFactory& nativeAlsaMixerOpenFactory()
  {
    static auto factory = NativeAlsaMixerOpenFactory{};
    return factory;
  }

  AlsaMixerSession::AlsaMixerSession(AlsaMixerOpenFactory& factory)
    : _factory{factory}
  {
  }

  bool AlsaMixerSession::tryInit(::snd_pcm_t* pcm)
  {
    auto const lock = std::scoped_lock{_handleMutex};
    _optElementId.reset();
    _ioPtr = _factory.open(pcm);

    if (!_ioPtr || !_ioPtr->tryRefresh())
    {
      _ioPtr.reset();
      _volumeMode.store(AlsaVolumeControlMode::SoftwareGain);
      publishRenderGainLocked();
      return false;
    }

    auto ids = _ioPtr->elementIds();
    std::ranges::stable_sort(ids, {}, [](AlsaMixerElementId const& id) { return mixerRank(id.name); });

    for (auto const& id : ids)
    {
      if (std::ranges::contains(_failedElementIds, id))
      {
        continue;
      }

      if (_ioPtr->inspect(id))
      {
        _optElementId = id;
        _softwareVolume = 1.0F;
        _volumeMode.store(AlsaVolumeControlMode::HardwareMixer);
        publishRenderGainLocked();
        return true;
      }
    }

    _ioPtr.reset();
    _volumeMode.store(AlsaVolumeControlMode::SoftwareGain);
    publishRenderGainLocked();
    return false;
  }

  void AlsaMixerSession::close()
  {
    auto const lock = std::scoped_lock{_handleMutex};
    _ioPtr.reset();
    _optElementId.reset();
    _volumeMode.store(AlsaVolumeControlMode::Unavailable);
    publishRenderGainLocked();
  }

  Result<> AlsaMixerSession::setVolume(float volume)
  {
    if (std::isnan(volume))
    {
      return makeError(Error::Code::InvalidInput, "ALSA mixer volume request must not be NaN");
    }

    auto const lock = std::scoped_lock{_handleMutex};
    float const normalized = std::clamp(volume, 0.0F, 1.0F);

    if (_volumeMode.load() != AlsaVolumeControlMode::HardwareMixer)
    {
      _softwareVolume = normalized;
      publishRenderGainLocked();
      return {};
    }

    auto const optSnapshot = refreshSelected();

    if (!optSnapshot)
    {
      return makeError(Error::Code::IoError,
                       "ALSA mixer refresh or element relocation failed; no volume write was attempted; using unity "
                       "software fallback");
    }

    // Allocate the failure record before touching hardware; discard it only after a successful write.
    _failedElementIds.push_back(optSnapshot->id);
    bool written = false;

    if (auto const optRange = usableDecibelRange(*optSnapshot); optRange)
    {
      long const level = quantizedLevel(*optRange, normalized);
      written = _ioPtr->trySetDecibelVolume(optSnapshot->id, level);
    }
    else
    {
      long const level = quantizedLevel(optSnapshot->rawRange, normalized);
      written = _ioPtr->trySetRawVolume(optSnapshot->id, level);
    }

    if (!written)
    {
      useSoftwareGainAfterHardwareUncertainty();
      return makeError(
        Error::Code::IoError,
        "ALSA mixer volume write failed; hardware state may have changed partially; using unity software fallback");
    }

    _failedElementIds.pop_back();
    return {};
  }

  AlsaMixerStateSnapshot AlsaMixerSession::setMuted(bool muted)
  {
    auto const lock = std::scoped_lock{_handleMutex};
    _applicationMuted = muted;
    publishRenderGainLocked();
    return stateSnapshotLocked();
  }

  AlsaMixerStateSnapshot AlsaMixerSession::stateSnapshot()
  {
    auto const lock = std::scoped_lock{_handleMutex};
    return stateSnapshotLocked();
  }

  bool AlsaMixerSession::isApplicationMuted() const
  {
    auto const lock = std::scoped_lock{_handleMutex};
    return _applicationMuted;
  }

  AlsaVolumeControlMode AlsaMixerSession::volumeMode() const noexcept
  {
    return _volumeMode.load();
  }

  float AlsaMixerSession::renderGain() const noexcept
  {
    return _renderGain.load();
  }

  std::optional<AlsaMixerElementSnapshot> AlsaMixerSession::refreshSelected()
  {
    if (!_ioPtr || !_optElementId || !_ioPtr->tryRefresh())
    {
      useSoftwareGainAfterHardwareUncertainty();
      return std::nullopt;
    }

    auto optSnapshot = _ioPtr->inspect(*_optElementId);

    if (!optSnapshot)
    {
      useSoftwareGainAfterHardwareUncertainty();
      return std::nullopt;
    }

    return optSnapshot;
  }

  AlsaMixerStateSnapshot AlsaMixerSession::stateSnapshotLocked()
  {
    if (_volumeMode.load() != AlsaVolumeControlMode::HardwareMixer)
    {
      return {.volume = _softwareVolume,
              .applicationMuted = _applicationMuted,
              .effectiveMuted = _applicationMuted,
              .volumeMode = _volumeMode.load()};
    }

    auto const optSnapshot = refreshSelected();

    if (!optSnapshot)
    {
      return {.volume = _softwareVolume,
              .applicationMuted = _applicationMuted,
              .effectiveMuted = _applicationMuted,
              .volumeMode = _volumeMode.load()};
    }

    float volume = normalizedLevel(optSnapshot->rawRange, optSnapshot->rawLevels.front());

    if (auto const optRange = usableDecibelRange(*optSnapshot); optRange && !optSnapshot->decibelLevels.empty())
    {
      volume = normalizedLevel(*optRange, optSnapshot->decibelLevels.front());
    }

    return {.volume = volume,
            .applicationMuted = _applicationMuted,
            .effectiveMuted = _applicationMuted || optSnapshot->optHardwareMuted.value_or(false),
            .volumeMode = AlsaVolumeControlMode::HardwareMixer};
  }

  void AlsaMixerSession::useSoftwareGainAfterHardwareUncertainty() noexcept
  {
    _ioPtr.reset();
    _optElementId.reset();
    _softwareVolume = 1.0F;
    _volumeMode.store(AlsaVolumeControlMode::SoftwareGain);
    publishRenderGainLocked();
  }

  void AlsaMixerSession::publishRenderGainLocked() noexcept
  {
    float const volumeGain = _volumeMode.load() == AlsaVolumeControlMode::SoftwareGain ? _softwareVolume : 1.0F;
    _renderGain.store(_applicationMuted ? 0.0F : volumeGain);
  }
} // namespace ao::audio::backend::detail
