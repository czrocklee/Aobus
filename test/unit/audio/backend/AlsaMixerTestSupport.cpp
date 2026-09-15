// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlsaMixerTestSupport.h"

#include "lib/audio/backend/AlsaExclusiveBackend.h"
#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#include "lib/audio/backend/detail/AlsaMixerSession.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/Property.h>
#include <ao/audio/SignalFormat.h>

extern "C"
{
#include <alsa/asoundlib.h>
}
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    FakeMixerElement* findElement(std::vector<FakeMixerElement>& elements, AlsaMixerElementId const& id)
    {
      auto const it = std::ranges::find(elements, id, &FakeMixerElement::id);
      return it == elements.end() ? nullptr : std::addressof(*it);
    }

    FakeMixerElement const* findElement(std::vector<FakeMixerElement> const& elements, AlsaMixerElementId const& id)
    {
      auto const it = std::ranges::find(elements, id, &FakeMixerElement::id);
      return it == elements.end() ? nullptr : std::addressof(*it);
    }

    class FakeMixerIo final : public AlsaMixerIo
    {
    public:
      explicit FakeMixerIo(std::shared_ptr<FakeMixerState> statePtr)
        : _statePtr{std::move(statePtr)}
      {
        _statePtr->cachedElements = _statePtr->hardwareElements;
        _statePtr->optCachedHardwareMuted = _statePtr->optHardwareMuted;
      }

      ~FakeMixerIo() override { ++_statePtr->closeCount; }

      FakeMixerIo(FakeMixerIo const&) = delete;
      FakeMixerIo& operator=(FakeMixerIo const&) = delete;
      FakeMixerIo(FakeMixerIo&&) = delete;
      FakeMixerIo& operator=(FakeMixerIo&&) = delete;

      bool tryRefresh() override
      {
        ++_statePtr->refreshCount;

        if (_statePtr->beforeRefresh)
        {
          _statePtr->beforeRefresh();
        }

        if (!_statePtr->refreshSucceeds)
        {
          return false;
        }

        _statePtr->cachedElements = _statePtr->hardwareElements;
        _statePtr->optCachedHardwareMuted = _statePtr->optHardwareMuted;
        return true;
      }

      std::vector<AlsaMixerElementId> elementIds() const override
      {
        auto ids = std::vector<AlsaMixerElementId>{};

        for (auto const& element : _statePtr->cachedElements)
        {
          ids.push_back(element.id);
        }

        return ids;
      }

      std::optional<AlsaMixerElementSnapshot> inspect(AlsaMixerElementId const& id) const override
      {
        ++_statePtr->inspectCount;
        auto const* const element = findElement(std::as_const(_statePtr->cachedElements), id);

        if (element == nullptr || !element->active || !element->readable ||
            element->rawRange.max <= element->rawRange.min || element->rawLevels.empty() ||
            std::ranges::any_of(element->rawLevels,
                                [&](AlsaMixerLevel level)
                                { return level < element->rawRange.min || level > element->rawRange.max; }))
        {
          return std::nullopt;
        }

        auto optDecibelRange = element->optDecibelRange;
        auto decibelLevels = element->decibelLevels;

        if (optDecibelRange &&
            (optDecibelRange->max <= optDecibelRange->min || decibelLevels.size() != element->rawLevels.size()))
        {
          optDecibelRange.reset();
          decibelLevels.clear();
        }

        return AlsaMixerElementSnapshot{.id = element->id,
                                        .rawRange = element->rawRange,
                                        .rawLevels = element->rawLevels,
                                        .optDecibelRange = optDecibelRange,
                                        .decibelLevels = std::move(decibelLevels),
                                        .optHardwareMuted = _statePtr->optCachedHardwareMuted};
      }

      bool trySetRawVolume(AlsaMixerElementId const& id, long level) override { return tryWrite(id, level, false); }

      bool trySetDecibelVolume(AlsaMixerElementId const& id, long level) override { return tryWrite(id, level, true); }

    private:
      bool tryWrite(AlsaMixerElementId const& id, AlsaMixerLevel level, bool decibels)
      {
        ++_statePtr->writeCount;
        _statePtr->writtenLevels.push_back(level);
        auto* const cached = findElement(_statePtr->cachedElements, id);
        auto* const hardware = findElement(_statePtr->hardwareElements, id);

        if (cached == nullptr || hardware == nullptr)
        {
          return false;
        }

        _statePtr->writeGenerations.push_back(cached->generation);
        auto& cachedLevels = decibels ? cached->decibelLevels : cached->rawLevels;
        auto& hardwareLevels = decibels ? hardware->decibelLevels : hardware->rawLevels;

        if (!_statePtr->writeSucceeds)
        {
          if (_statePtr->applyFirstChannelBeforeWriteFailure && !hardwareLevels.empty())
          {
            hardwareLevels.front() = level;
            cachedLevels.front() = level;
          }

          return false;
        }

        std::ranges::fill(hardwareLevels, level);
        std::ranges::fill(cachedLevels, level);
        // A simple-element volume write can also submit cached switch state.
        _statePtr->optHardwareMuted = _statePtr->optCachedHardwareMuted;
        return true;
      }

      std::shared_ptr<FakeMixerState> _statePtr;
    };
  } // namespace

  FakeMixerOpenFactory::FakeMixerOpenFactory(std::shared_ptr<FakeMixerState> statePtr)
    : _statePtr{std::move(statePtr)}
  {
  }

  std::unique_ptr<AlsaMixerIo> FakeMixerOpenFactory::open(::snd_pcm_t* /*pcm*/)
  {
    ++_statePtr->openCount;

    if (!_statePtr->openSucceeds)
    {
      return nullptr;
    }

    return std::make_unique<FakeMixerIo>(_statePtr);
  }

  void MixerFixture::addElement(FakeMixerElement element)
  {
    statePtr->hardwareElements.push_back(std::move(element));
  }

  void MixerFixture::initialize()
  {
    REQUIRE(session.tryInit(nullptr));
  }

  struct AlsaControlBackend::Impl final
  {
    FakeMixerOpenFactory factory;
    AlsaMixerSession* mixer = nullptr;
    std::unique_ptr<::ao::audio::backend::AlsaExclusiveBackend> controlsPtr;

    Impl(Device const& device, AlsaGraphPublisher publisher, std::shared_ptr<FakeMixerState> mixerStatePtr)
      : factory{std::move(mixerStatePtr)}
    {
      auto mixerPtr = std::make_unique<AlsaMixerSession>(factory);
      mixer = mixerPtr.get();
      std::ignore = mixer->tryInit(nullptr);
      controlsPtr = std::make_unique<::ao::audio::backend::AlsaExclusiveBackend>(
        device, kProfileExclusive, std::move(publisher), std::move(mixerPtr));
    }
  };

  AlsaControlBackend::AlsaControlBackend(Device const& device,
                                         AlsaGraphPublisher publisher,
                                         std::shared_ptr<FakeMixerState> mixerStatePtr)
    : _implPtr{std::make_unique<Impl>(device, std::move(publisher), std::move(mixerStatePtr))}
  {
  }

  AlsaControlBackend::~AlsaControlBackend() = default;

  std::optional<PcmFormat> AlsaControlBackend::prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept
  {
    return _implPtr->controlsPtr->prewarmFormatHint(sourceFormat);
  }

  Result<OpenedPcmMode> AlsaControlBackend::open(SignalFormat const& sourceFormat, RenderTarget& /*target*/)
  {
    std::ignore = _implPtr->mixer->tryInit(nullptr);
    return OpenedPcmMode{.clientFormat = pcmFormat(sourceFormat, SampleEncoding::Signed16Le)};
  }

  void AlsaControlBackend::start()
  {
  }

  void AlsaControlBackend::pause()
  {
  }

  void AlsaControlBackend::resume()
  {
  }

  void AlsaControlBackend::flush()
  {
  }

  void AlsaControlBackend::stop()
  {
  }

  void AlsaControlBackend::close()
  {
    _implPtr->controlsPtr->close();
  }

  Result<> AlsaControlBackend::setProperty(PropertyId id, PropertyValue const& value)
  {
    return _implPtr->controlsPtr->setProperty(id, value);
  }

  Result<PropertyValue> AlsaControlBackend::property(PropertyId id) const
  {
    return _implPtr->controlsPtr->property(id);
  }

  PropertyInfo AlsaControlBackend::queryProperty(PropertyId id) const noexcept
  {
    return _implPtr->controlsPtr->queryProperty(id);
  }

  BackendId AlsaControlBackend::backendId() const
  {
    return kBackendAlsa;
  }

  ProfileId AlsaControlBackend::profileId() const
  {
    return kProfileExclusive;
  }

  bool AlsaControlBackend::isMixerInitialized() const noexcept
  {
    return _implPtr->mixer->volumeMode() == AlsaVolumeControlMode::HardwareMixer;
  }
} // namespace ao::audio::backend::detail::test
