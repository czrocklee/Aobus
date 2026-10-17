// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlsaChannelMap.h"

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/utility/Raii.h>

extern "C"
{
#include <alsa/pcm.h>
}

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <utility>

namespace ao::audio::backend::detail
{
  namespace
  {
    constexpr std::size_t kMaxMaps = 256;

    // WAVE speaker bits name the application set, not ALSA enum ordinals.
    enum class WaveSpeakerBit : std::uint32_t
    {
      FrontLeft = 0x1,
      FrontRight = 0x2,
      FrontCenter = 0x4,
      LowFrequency = 0x8,
      BackLeft = 0x10,
      BackRight = 0x20,
      FrontLeftCenter = 0x40,
      FrontRightCenter = 0x80,
      BackCenter = 0x100,
      SideLeft = 0x200,
      SideRight = 0x400,
      TopCenter = 0x800,
      TopFrontLeft = 0x1000,
      TopFrontCenter = 0x2000,
      TopFrontRight = 0x4000,
      TopBackLeft = 0x8000,
      TopBackCenter = 0x10000,
      TopBackRight = 0x20000,
    };

    std::unexpected<BackendOpenFailure> malformedMap(char const* context)
    {
      return std::unexpected{BackendOpenFailure{.error = makeError(Error::Code::InitFailed, context).error(),
                                                .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    std::unexpected<BackendOpenFailure> incompatibleMap()
    {
      return std::unexpected{BackendOpenFailure{
        .error =
          makeError(Error::Code::FormatRejected,
                    "ALSA current channel positions conflict with the source; no lossless slot mapping is available")
            .error(),
        .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
    }

    std::uint32_t speakerBit(std::uint32_t position) noexcept
    {
      switch (position)
      {
        case SND_CHMAP_FL: return std::to_underlying(WaveSpeakerBit::FrontLeft);
        case SND_CHMAP_FR: return std::to_underlying(WaveSpeakerBit::FrontRight);
        case SND_CHMAP_FC: return std::to_underlying(WaveSpeakerBit::FrontCenter);
        case SND_CHMAP_LFE: return std::to_underlying(WaveSpeakerBit::LowFrequency);
        case SND_CHMAP_RL: return std::to_underlying(WaveSpeakerBit::BackLeft);
        case SND_CHMAP_RR: return std::to_underlying(WaveSpeakerBit::BackRight);
        case SND_CHMAP_FLC: return std::to_underlying(WaveSpeakerBit::FrontLeftCenter);
        case SND_CHMAP_FRC: return std::to_underlying(WaveSpeakerBit::FrontRightCenter);
        case SND_CHMAP_RC: return std::to_underlying(WaveSpeakerBit::BackCenter);
        case SND_CHMAP_SL: return std::to_underlying(WaveSpeakerBit::SideLeft);
        case SND_CHMAP_SR: return std::to_underlying(WaveSpeakerBit::SideRight);
        case SND_CHMAP_TC: return std::to_underlying(WaveSpeakerBit::TopCenter);
        case SND_CHMAP_TFL: return std::to_underlying(WaveSpeakerBit::TopFrontLeft);
        case SND_CHMAP_TFC: return std::to_underlying(WaveSpeakerBit::TopFrontCenter);
        case SND_CHMAP_TFR: return std::to_underlying(WaveSpeakerBit::TopFrontRight);
        case SND_CHMAP_TRL: return std::to_underlying(WaveSpeakerBit::TopBackLeft);
        case SND_CHMAP_TRC: return std::to_underlying(WaveSpeakerBit::TopBackCenter);
        case SND_CHMAP_TRR: return std::to_underlying(WaveSpeakerBit::TopBackRight);
        default: return 0;
      }
    }

    bool hasValidShape(std::span<std::uint32_t const> positions) noexcept
    {
      if (positions.empty() || positions.size() > kAlsaChannelCapacity)
      {
        return false;
      }

      for (std::size_t i = 0; i < positions.size(); ++i)
      {
        auto const position = positions[i];
        auto const role = position & SND_CHMAP_POSITION_MASK;

        if ((position & ~(SND_CHMAP_POSITION_MASK | SND_CHMAP_PHASE_INVERSE | SND_CHMAP_DRIVER_SPEC)) != 0U ||
            ((position & SND_CHMAP_DRIVER_SPEC) == 0U && role > SND_CHMAP_LAST))
        {
          return false;
        }

        // UNKNOWN and NA may describe several unassigned slots. Repeating an
        // actual role (including MONO) is not a one-to-one successful map.
        if (role != SND_CHMAP_UNKNOWN && role != SND_CHMAP_NA)
        {
          for (std::size_t j = 0; j < i; ++j)
          {
            if ((positions[j] & SND_CHMAP_POSITION_MASK) == role)
            {
              return false;
            }
          }
        }
      }

      return true;
    }

    std::expected<void, BackendOpenFailure> classifySupportedMaps(AlsaChannelMap& map)
    {
      if (map.observations.supportedMaps.size() > kMaxMaps)
      {
        return malformedMap("ALSA capability query returned a malformed channel map list");
      }

      for (auto const& supported : map.observations.supportedMaps)
      {
        if (supported.empty() || supported.size() > kAlsaChannelCapacity)
        {
          return malformedMap("ALSA capability query returned a malformed channel map");
        }

        // An offer of another channel count never describes this stream, so
        // its role semantics cannot fault the applied current mode. Only a
        // channel-count-matching capability could become the negotiated map
        // and is validated before it can veto the open.
        if (supported.size() != map.channels)
        {
          continue;
        }

        if (!hasValidShape(supported))
        {
          return malformedMap("ALSA capability query returned a malformed channel map");
        }

        map.observations.query = AlsaChannelMapObservations::Query::ApplicableMap;
      }

      return {};
    }

    bool hasCompatiblePartialPositions(std::span<std::uint32_t const> positions, std::uint32_t sourceMask) noexcept
    {
      // Partial maps cannot justify a permutation. Retain compatible unassigned
      // slots, but do not erase a known conflicting or unrepresentable role.
      // The caller has already validated the complete source count and mask.
      auto remainingMask = sourceMask;

      for (auto const position : positions)
      {
        auto const sourceBit = remainingMask & (0U - remainingMask);
        remainingMask &= remainingMask - 1U;
        // NA is an explicit silent slot, unlike UNKNOWN position evidence.
        // MONO deliberately carries no unconditional FC meaning.
        bool const unassigned = position == SND_CHMAP_UNKNOWN || position == SND_CHMAP_MONO;

        if (!unassigned && speakerBit(position) != sourceBit)
        {
          return false;
        }
      }

      return true;
    }
  } // namespace

  std::expected<AlsaChannelMapObservations, BackendOpenFailure> readAlsaChannelMaps(::snd_pcm_t* const pcm)
  {
    auto observations = AlsaChannelMapObservations{};
    auto mapsPtr = utility::makeUniquePtr<::snd_pcm_free_chmaps>(::snd_pcm_query_chmaps(pcm));

    if (mapsPtr)
    {
      observations.query = AlsaChannelMapObservations::Query::NoApplicableMap;
      std::size_t index = 0;

      for (; index < kMaxMaps && mapsPtr.get()[index] != nullptr; ++index)
      {
        auto const& map = mapsPtr.get()[index]->map;

        if (map.channels == 0U || map.channels > kAlsaChannelCapacity)
        {
          return malformedMap("ALSA capability query returned an invalid channel-map count");
        }

        auto const positions = std::span<unsigned int const>{&map.pos[0], static_cast<std::size_t>(map.channels)};
        observations.supportedMaps.emplace_back(positions.begin(), positions.end());
      }

      if (index == kMaxMaps && mapsPtr.get()[index] != nullptr)
      {
        return malformedMap("ALSA capability query exceeded the bounded channel-map list");
      }
    }

    auto currentPtr = std::unique_ptr<::snd_pcm_chmap_t, decltype(&std::free)>{::snd_pcm_get_chmap(pcm), &std::free};

    if (currentPtr)
    {
      if (currentPtr->channels == 0U || currentPtr->channels > kAlsaChannelCapacity)
      {
        return malformedMap("ALSA current readback returned an invalid channel-map count");
      }

      auto const positions =
        std::span<unsigned int const>{&currentPtr->pos[0], static_cast<std::size_t>(currentPtr->channels)};
      observations.optCurrentPositions.emplace(positions.begin(), positions.end());
    }

    return observations;
  }

  std::expected<AlsaChannelMap, BackendOpenFailure> prepareAlsaChannelMap(PcmFormat const& clientFormat,
                                                                          AlsaChannelMapReader const& readMaps)
  {
    auto observationsRes = readMaps();

    if (!observationsRes)
    {
      return std::unexpected{observationsRes.error()};
    }

    auto map = AlsaChannelMap{.observations = std::move(*observationsRes),
                              .channels = clientFormat.channels,
                              .sampleBytes = static_cast<std::uint8_t>(bytesPerSample(clientFormat.encoding))};

    if (map.channels == 0U || map.sampleBytes == 0U)
    {
      return malformedMap("ALSA channel mapping requires validated PCM storage");
    }

    for (std::size_t slot = 0; slot < map.channels; ++slot)
    {
      map.sourceSlots[slot] = static_cast<std::uint8_t>(slot);
    }

    if (auto const supportedRes = classifySupportedMaps(map); !supportedRes)
    {
      return std::unexpected{supportedRes.error()};
    }

    if (!map.observations.optCurrentPositions)
    {
      return map;
    }

    auto const& positions = *map.observations.optCurrentPositions;

    if (positions.size() != map.channels || !hasValidShape(positions))
    {
      return malformedMap("ALSA current readback returned a malformed channel map");
    }

    bool const sourceIdentified = clientFormat.speakerMask != 0U;
    std::uint32_t knownMask = 0;
    bool complete = true;

    for (auto const position : positions)
    {
      // An identified source needs a lossless one-to-one slot mapping, so an
      // inverted-polarity slot stays a contextual refusal. An unidentified
      // source never permutes: the flagged observation remains usable legacy
      // playback without claiming verified polarity or endpoint identity.
      if (sourceIdentified && (position & SND_CHMAP_PHASE_INVERSE) != 0U)
      {
        return incompatibleMap();
      }

      auto const bit = speakerBit(position);
      knownMask |= bit;
      complete = complete && bit != 0U;
    }

    // Only complete observed positions establish the endpoint set. A source
    // with mask zero remains unidentified even when the endpoint is known.
    map.delivery.nativeSpeakerMask = complete ? knownMask : 0U;

    if (!sourceIdentified)
    {
      return map;
    }

    if (std::popcount(clientFormat.speakerMask) != map.channels)
    {
      return incompatibleMap();
    }

    if (complete)
    {
      if (knownMask != clientFormat.speakerMask)
      {
        return incompatibleMap();
      }

      for (std::size_t slot = 0; slot < positions.size(); ++slot)
      {
        auto const bit = speakerBit(positions[slot]);
        auto const sourceSlot = static_cast<std::uint8_t>(std::popcount(clientFormat.speakerMask & (bit - 1U)));
        map.sourceSlots[slot] = sourceSlot;
        map.needsPermutation = map.needsPermutation || sourceSlot != slot;
      }

      map.delivery.isMapped = true;
      return map;
    }

    if (!hasCompatiblePartialPositions(positions, clientFormat.speakerMask))
    {
      return incompatibleMap();
    }

    return map;
  }

  void applyAlsaChannelPermutation(std::span<std::byte> const pcm,
                                   AlsaChannelMap const& map,
                                   std::span<std::byte> const frameScratch) noexcept
  {
    if (!map.needsPermutation)
    {
      return;
    }

    std::size_t const frameSize = static_cast<std::size_t>(map.channels) * map.sampleBytes;
    AO_RT_INVARIANT(frameSize != 0U && map.sampleBytes <= kAlsaMaxSampleBytes && pcm.size() % frameSize == 0U &&
                      frameScratch.size() >= frameSize,
                    "ALSA permutation requires whole frames and provisioned frame scratch");

    for (std::size_t offset = 0; offset < pcm.size(); offset += frameSize)
    {
      std::memcpy(frameScratch.data(), pcm.data() + offset, frameSize);

      for (std::size_t slot = 0; slot < map.channels; ++slot)
      {
        std::memcpy(pcm.data() + offset + (slot * map.sampleBytes),
                    frameScratch.data() + (static_cast<std::size_t>(map.sourceSlots[slot]) * map.sampleBytes),
                    map.sampleBytes);
      }
    }
  }
} // namespace ao::audio::backend::detail
