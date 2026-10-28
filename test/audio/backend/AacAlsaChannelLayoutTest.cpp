// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/AacDecoderSession.h"
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

  TEST_CASE("AacAlsaChannelLayout - mono FC identity constrains native observations without changing PCM")
  {
    auto decoderPtr = ao::test::requireValue(
      AacDecoderSession::open(audio::test::requireAudioFixture("aac-layout/mono.m4a"), SampleEncoding::Signed16Le));
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
    CHECK(decoderPtr->streamInfo() == info);

    // Parsed observations only: native channel-count admission precedes this stage.
    // No device is opened, no upmix is requested, and no physical routing is proved.
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
        CHECK(decoderPtr->streamInfo() == info);
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
      CHECK(unassignedRes->observations.optCurrentPositions == positions);
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
      CHECK(decoderPtr->streamInfo() == info);
    }
  }
} // namespace ao::audio::backend::test
