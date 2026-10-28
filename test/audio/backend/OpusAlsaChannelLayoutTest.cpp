// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/OpusDecoderSession.h"
#include "lib/audio/backend/detail/AlsaChannelMap.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <alsa/pcm.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    std::expected<detail::AlsaChannelMap, BackendOpenFailure> prepareMap(
      std::optional<std::vector<unsigned int>> optPositions,
      PcmFormat const& clientFormat)
    {
      return detail::prepareAlsaChannelMap(
        clientFormat,
        [&]
        {
          return std::expected<detail::AlsaChannelMapObservations, BackendOpenFailure>{
            detail::AlsaChannelMapObservations{.optCurrentPositions = optPositions}};
        });
    }
  } // namespace

  TEST_CASE("OpusAlsaChannelLayout - identified family roles constrain the selected native route")
  {
    auto decoderPtr = ao::test::requireValue(
      OpusDecoderSession::open(audio::test::requireAudioFixture("surround.opus"), SampleEncoding::Signed16Le));
    auto const info = decoderPtr->streamInfo();
    CHECK(info.sourceFormat == SignalFormat{.sampleRate = 48000,
                                            .channels = 6,
                                            .precisionBits = 16,
                                            .sampleKind = SampleKind::Integer,
                                            .speakerMask = 0x3F});
    CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Identified);
    REQUIRE(info.outputFormat ==
            PcmFormat{.sampleRate = 48000, .channels = 6, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3F});

    // Exercise the production parsed-observation boundary, not a device open.
    // The former zero-mask control below is a format value, not an old decoder.
    SECTION("A complete conflicting side set is refused where zero-mask transport was eligible")
    {
      auto const positions =
        std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_SL, SND_CHMAP_SR};
      auto const identifiedRes = prepareMap(positions, info.outputFormat);
      REQUIRE_FALSE(identifiedRes);
      CHECK(identifiedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK(identifiedRes.error().error.code == Error::Code::FormatRejected);

      auto unassigned = info.outputFormat;
      unassigned.speakerMask = 0;
      auto const unassignedRes = prepareMap(positions, unassigned);
      REQUIRE(unassignedRes);
      CHECK_FALSE(unassignedRes->needsPermutation);
      CHECK_FALSE(unassignedRes->delivery.isMapped);
      CHECK(unassignedRes->delivery.nativeSpeakerMask == 0x60F);
    }

    SECTION("The same set in native order privately permutes canonical Opus bytes")
    {
      auto const positions =
        std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_RL, SND_CHMAP_RR, SND_CHMAP_FC, SND_CHMAP_LFE};
      auto const mapRes = prepareMap(positions, info.outputFormat);
      REQUIRE(mapRes);
      CHECK(mapRes->needsPermutation);
      CHECK(mapRes->delivery.isMapped);
      CHECK(mapRes->delivery.nativeSpeakerMask == 0x3F);
      CHECK(mapRes->observations.optCurrentPositions == positions);
      // Canonical FL FR FC LFE BL BR -> native FL FR RL RR FC LFE.
      constexpr auto kNativeSourceSlots = std::to_array<std::uint8_t>({0, 1, 4, 5, 2, 3});

      for (std::size_t slot = 0; slot < kNativeSourceSlots.size(); ++slot)
      {
        CHECK(mapRes->sourceSlots[slot] == kNativeSourceSlots[slot]);
      }

      auto const blockRes = decoderPtr->readNextBlock();
      REQUIRE(blockRes);
      REQUIRE(blockRes->frames > 0);
      REQUIRE(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 12U);
      auto pcm = std::vector<std::byte>{blockRes->bytes.begin(), blockRes->bytes.end()};
      auto scratch = std::array<std::byte, detail::kAlsaPermutationScratchBytes>{};
      detail::applyAlsaChannelPermutation(pcm, *mapRes, scratch);

      for (std::size_t frame = 0; frame < blockRes->frames; ++frame)
      {
        for (std::size_t slot = 0; slot < kNativeSourceSlots.size(); ++slot)
        {
          auto const destination = (frame * 12U) + (slot * 2U);
          auto const source = (frame * 12U) + (static_cast<std::size_t>(kNativeSourceSlots[slot]) * 2U);
          CHECK(pcm[destination] == blockRes->bytes[source]);
          CHECK(pcm[destination + 1U] == blockRes->bytes[source + 1U]);
        }
      }
    }

    SECTION("Missing current observations retain eligibility without a mapping claim")
    {
      auto const mapRes = prepareMap(std::nullopt, info.outputFormat);
      REQUIRE(mapRes);
      CHECK_FALSE(mapRes->needsPermutation);
      CHECK_FALSE(mapRes->delivery.isMapped);
      CHECK(mapRes->delivery.nativeSpeakerMask == 0);
    }

    SECTION("Compatible partial positions do not require a complete observation")
    {
      auto const positions = std::vector<unsigned int>{
        SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_UNKNOWN, SND_CHMAP_UNKNOWN};
      auto const mapRes = prepareMap(positions, info.outputFormat);
      REQUIRE(mapRes);
      CHECK_FALSE(mapRes->needsPermutation);
      CHECK_FALSE(mapRes->delivery.isMapped);
      CHECK(mapRes->delivery.nativeSpeakerMask == 0);
    }

    SECTION("A known partial contradiction remains a track-route refusal")
    {
      auto const positions = std::vector<unsigned int>{
        SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_SL, SND_CHMAP_UNKNOWN};
      auto const mapRes = prepareMap(positions, info.outputFormat);
      REQUIRE_FALSE(mapRes);
      CHECK(mapRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK(mapRes.error().error.code == Error::Code::FormatRejected);
    }

    CHECK(decoderPtr->streamInfo() == info);
  }

  TEST_CASE("OpusAlsaChannelLayout - mono FC identity constrains native observations without changing PCM")
  {
    auto decoderPtr = ao::test::requireValue(
      OpusDecoderSession::open(audio::test::requireAudioFixture("mono.opus"), SampleEncoding::Signed16Le));
    auto const info = decoderPtr->streamInfo();
    CHECK(info.sourceFormat == SignalFormat{.sampleRate = 48000,
                                            .channels = 1,
                                            .precisionBits = 16,
                                            .sampleKind = SampleKind::Integer,
                                            .speakerMask = 0x4});
    CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Identified);
    REQUIRE(info.outputFormat ==
            PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x4});

    auto const blockRes = decoderPtr->readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->frames > 0);
    REQUIRE(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 2U);
    auto const originalPcm = std::vector<std::byte>{blockRes->bytes.begin(), blockRes->bytes.end()};

    // Parsed observations only: native channel-count admission precedes this stage.
    // This does not open a device or make a stereo-only raw PCM accept mono.
    SECTION("FC maps while ambiguous or missing positions remain eligible without invented roles")
    {
      struct Observation final
      {
        std::optional<std::vector<unsigned int>> optPositions;
        bool isMapped = false;
      };
      auto const observations =
        std::array{Observation{.optPositions = std::vector<unsigned int>{SND_CHMAP_FC}, .isMapped = true},
                   Observation{.optPositions = std::vector<unsigned int>{SND_CHMAP_MONO}, .isMapped = false},
                   Observation{.optPositions = std::vector<unsigned int>{SND_CHMAP_UNKNOWN}, .isMapped = false},
                   Observation{.optPositions = std::nullopt, .isMapped = false}};

      for (auto const& observation : observations)
      {
        CAPTURE(observation.optPositions);
        auto const mapRes = prepareMap(observation.optPositions, info.outputFormat);
        REQUIRE(mapRes);
        CHECK(mapRes->observations.optCurrentPositions == observation.optPositions);
        CHECK(mapRes->channels == 1);
        CHECK(mapRes->sampleBytes == 2);
        CHECK_FALSE(mapRes->needsPermutation);
        CHECK(mapRes->delivery.isMapped == observation.isMapped);
        CHECK(mapRes->delivery.nativeSpeakerMask == (observation.isMapped ? 0x4U : 0U));

        auto pcm = originalPcm;
        auto scratch = std::array<std::byte, detail::kAlsaPermutationScratchBytes>{};
        detail::applyAlsaChannelPermutation(pcm, *mapRes, scratch);
        CHECK(pcm == originalPcm);
        CHECK(pcm.size() == static_cast<std::size_t>(blockRes->frames) * 2U);
      }
    }

    SECTION("An explicit FL singleton refuses identified FC but accepts the historical zero-mask value")
    {
      auto const positions = std::vector<unsigned int>{SND_CHMAP_FL};
      auto const identifiedRes = prepareMap(positions, info.outputFormat);
      REQUIRE_FALSE(identifiedRes);
      CHECK(identifiedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK(identifiedRes.error().error.code == Error::Code::FormatRejected);

      // The control changes only the format value, not the decoder implementation.
      auto unassigned = info.outputFormat;
      unassigned.speakerMask = 0;
      auto const unassignedRes = prepareMap(positions, unassigned);
      REQUIRE(unassignedRes);
      CHECK(unassignedRes->channels == 1);
      CHECK(unassignedRes->sampleBytes == 2);
      CHECK_FALSE(unassignedRes->needsPermutation);
      CHECK_FALSE(unassignedRes->delivery.isMapped);
      CHECK(unassignedRes->delivery.nativeSpeakerMask == 0x1);

      auto pcm = originalPcm;
      auto scratch = std::array<std::byte, detail::kAlsaPermutationScratchBytes>{};
      detail::applyAlsaChannelPermutation(pcm, *unassignedRes, scratch);
      CHECK(pcm == originalPcm);
      CHECK(pcm.size() == static_cast<std::size_t>(blockRes->frames) * 2U);
    }

    CHECK(decoderPtr->streamInfo() == info);
  }
} // namespace ao::audio::backend::test
