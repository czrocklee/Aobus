// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaChannelMap.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

extern "C"
{
#include <alsa/pcm.h>
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    PcmFormat surroundFormat(std::uint32_t mask)
    {
      return PcmFormat{.sampleRate = 48000, .channels = 8, .encoding = SampleEncoding::Signed16Le, .speakerMask = mask};
    }

    std::vector<unsigned int> legacySurroundPositions()
    {
      // Linux's old UAC1 table for USB role bits 0x63F, not an inferred
      // rear/side assignment or a claim about the connected hardware.
      return {SND_CHMAP_FL,
              SND_CHMAP_FR,
              SND_CHMAP_FC,
              SND_CHMAP_LFE,
              SND_CHMAP_SL,
              SND_CHMAP_SR,
              SND_CHMAP_SL,
              SND_CHMAP_SR};
    }

    std::vector<unsigned int> modernSurroundPositions()
    {
      return {SND_CHMAP_FL,
              SND_CHMAP_FR,
              SND_CHMAP_FC,
              SND_CHMAP_LFE,
              SND_CHMAP_RL,
              SND_CHMAP_RR,
              SND_CHMAP_SL,
              SND_CHMAP_SR};
    }
  } // namespace

  TEST_CASE("AlsaChannelMapObservations - defaults preserve independent availability",
            "[audio][unit][alsa-channel-map]")
  {
    auto const defaults = AlsaChannelMapObservations{};
    CHECK(defaults.query == AlsaChannelMapObservations::Query::Unavailable);
    CHECK(defaults.supportedMaps.empty());
    CHECK_FALSE(defaults.optCurrentPositions);

    auto const noApplicableMap =
      AlsaChannelMapObservations{.query = AlsaChannelMapObservations::Query::NoApplicableMap};
    CHECK(noApplicableMap.query == AlsaChannelMapObservations::Query::NoApplicableMap);
    CHECK(noApplicableMap.supportedMaps.empty());
    CHECK_FALSE(noApplicableMap.optCurrentPositions);

    auto const emptyCurrentPositions = AlsaChannelMapObservations{.optCurrentPositions = std::vector<unsigned int>{}};
    REQUIRE(emptyCurrentPositions.optCurrentPositions);
    CHECK(emptyCurrentPositions.optCurrentPositions->empty());
  }

  TEST_CASE("AlsaChannelMap - legacy UAC1 ambiguity preserves unidentified bytes without mapping proof",
            "[audio][unit][alsa-channel-map]")
  {
    bool const hasOffer = GENERATE(false, true);
    auto const client = surroundFormat(0);
    auto observations = AlsaChannelMapObservations{.optCurrentPositions = legacySurroundPositions()};

    if (hasOffer)
    {
      observations.supportedMaps.push_back(legacySurroundPositions());
    }

    auto const mapRes = prepareAlsaChannelMap(
      client, [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });

    REQUIRE(mapRes);
    CHECK(client.speakerMask == 0U);
    CHECK(mapRes->observations.optCurrentPositions == observations.optCurrentPositions);
    CHECK_FALSE(mapRes->delivery.isMapped);
    CHECK_FALSE(mapRes->delivery.hasSharedConversion);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0U);
    CHECK_FALSE(mapRes->needsPermutation);
    CHECK(mapRes->channels == 8U);
    CHECK(mapRes->sampleBytes == 2U);
    auto const expectedSlots = std::to_array<std::uint8_t>({0, 1, 2, 3, 4, 5, 6, 7});
    CHECK(std::ranges::equal(std::span<std::uint8_t const>{mapRes->sourceSlots}.first(8), expectedSlots));

    auto pcm = std::to_array<std::byte>(
      {std::byte{0x01}, std::byte{0x10}, std::byte{0x02}, std::byte{0x20}, std::byte{0x03}, std::byte{0x30},
       std::byte{0x04}, std::byte{0x40}, std::byte{0x05}, std::byte{0x50}, std::byte{0x06}, std::byte{0x60},
       std::byte{0x07}, std::byte{0x70}, std::byte{0x08}, std::byte{0x80}, std::byte{0x11}, std::byte{0x01},
       std::byte{0x12}, std::byte{0x02}, std::byte{0x13}, std::byte{0x03}, std::byte{0x14}, std::byte{0x04},
       std::byte{0x15}, std::byte{0x05}, std::byte{0x16}, std::byte{0x06}, std::byte{0x17}, std::byte{0x07},
       std::byte{0x18}, std::byte{0x08}});
    auto const expected = pcm;
    auto scratch = std::array<std::byte, kAlsaPermutationScratchBytes>{};
    applyAlsaChannelPermutation(pcm, *mapRes, scratch);
    CHECK(pcm == expected);
  }

  TEST_CASE("AlsaChannelMap - legacy UAC1 current positions cannot identify a known source",
            "[audio][unit][alsa-channel-map]")
  {
    auto const mask = GENERATE(0x63FU, 0xFFU);
    auto const mapRes =
      prepareAlsaChannelMap(surroundFormat(mask),
                            []
                            {
                              return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{
                                AlsaChannelMapObservations{.optCurrentPositions = legacySurroundPositions()}};
                            });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(mapRes.error().error.code == Error::Code::FormatRejected);
    CHECK(mapRes.error().error.message == "ALSA legacy UAC1 channel map has ambiguous surround positions");
  }

  TEST_CASE("AlsaChannelMap - a legacy UAC1 offer cannot replace unavailable current evidence",
            "[audio][unit][alsa-channel-map]")
  {
    auto const mask = GENERATE(0U, 0x63FU);
    auto const mapRes =
      prepareAlsaChannelMap(surroundFormat(mask),
                            []
                            {
                              return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{
                                AlsaChannelMapObservations{.supportedMaps = {legacySurroundPositions()}}};
                            });

    REQUIRE(mapRes);
    CHECK(mapRes->observations.query == AlsaChannelMapObservations::Query::ApplicableMap);
    CHECK_FALSE(mapRes->observations.optCurrentPositions);
    CHECK_FALSE(mapRes->delivery.isMapped);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0U);
    CHECK_FALSE(mapRes->needsPermutation);
  }

  TEST_CASE("AlsaChannelMap - modern current positions own permutation despite a legacy UAC1 offer",
            "[audio][unit][alsa-channel-map]")
  {
    bool const permuted = GENERATE(false, true);
    auto positions = modernSurroundPositions();

    if (permuted)
    {
      std::swap(positions[0], positions[1]);
      std::swap(positions[4], positions[5]);
      std::swap(positions[6], positions[7]);
    }

    auto observations = AlsaChannelMapObservations{
      .supportedMaps = {legacySurroundPositions()}, .optCurrentPositions = std::move(positions)};
    auto const mapRes = prepareAlsaChannelMap(
      surroundFormat(0x63F),
      [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });

    REQUIRE(mapRes);
    CHECK(mapRes->delivery.isMapped);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0x63FU);
    CHECK(mapRes->needsPermutation == permuted);
    auto const expectedSlots = permuted ? std::to_array<std::uint8_t>({1, 0, 2, 3, 5, 4, 7, 6})
                                        : std::to_array<std::uint8_t>({0, 1, 2, 3, 4, 5, 6, 7});
    CHECK(std::ranges::equal(std::span<std::uint8_t const>{mapRes->sourceSlots}.first(8), expectedSlots));

    auto pcm = std::to_array<std::byte>({std::byte{0x01},
                                         std::byte{0x10},
                                         std::byte{0x02},
                                         std::byte{0x20},
                                         std::byte{0x03},
                                         std::byte{0x30},
                                         std::byte{0x04},
                                         std::byte{0x40},
                                         std::byte{0x05},
                                         std::byte{0x50},
                                         std::byte{0x06},
                                         std::byte{0x60},
                                         std::byte{0x07},
                                         std::byte{0x70},
                                         std::byte{0x08},
                                         std::byte{0x80}});
    auto const expected = permuted ? std::to_array<std::byte>({std::byte{0x02},
                                                               std::byte{0x20},
                                                               std::byte{0x01},
                                                               std::byte{0x10},
                                                               std::byte{0x03},
                                                               std::byte{0x30},
                                                               std::byte{0x04},
                                                               std::byte{0x40},
                                                               std::byte{0x06},
                                                               std::byte{0x60},
                                                               std::byte{0x05},
                                                               std::byte{0x50},
                                                               std::byte{0x08},
                                                               std::byte{0x80},
                                                               std::byte{0x07},
                                                               std::byte{0x70}})
                                   : pcm;
    auto scratch = std::array<std::byte, kAlsaPermutationScratchBytes>{};
    applyAlsaChannelPermutation(pcm, *mapRes, scratch);
    CHECK(pcm == expected);
  }

  TEST_CASE("AlsaChannelMap - near legacy UAC1 current signatures remain malformed", "[audio][unit][alsa-channel-map]")
  {
    auto positions = legacySurroundPositions();

    SECTION("reordered duplicate roles do not match")
    {
      std::swap(positions[0], positions[1]);
    }

    SECTION("a different duplicate pair does not match")
    {
      positions[4] = SND_CHMAP_FL;
    }

    SECTION("a missing slot cannot borrow the compatibility rule")
    {
      positions.pop_back();
    }

    SECTION("an extra slot cannot borrow the compatibility rule")
    {
      positions.push_back(SND_CHMAP_UNKNOWN);
    }

    SECTION("polarity flags are not stripped for recognition")
    {
      positions[0] |= SND_CHMAP_PHASE_INVERSE;
    }

    SECTION("driver namespaces are not stripped for recognition")
    {
      positions[0] |= SND_CHMAP_DRIVER_SPEC;
    }

    SECTION("invalid flag storage stays invalid")
    {
      positions[0] |= 0x80000000U;
    }

    auto const mapRes = prepareAlsaChannelMap(surroundFormat(0),
                                              [&]
                                              {
                                                return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{
                                                  AlsaChannelMapObservations{.optCurrentPositions = positions}};
                                              });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(mapRes.error().error.code == Error::Code::InitFailed);
    CHECK(mapRes.error().error.message == "ALSA current readback returned a malformed channel map");
  }

  TEST_CASE("AlsaChannelMap - legacy UAC1 current evidence cannot hide independent capability faults",
            "[audio][unit][alsa-channel-map]")
  {
    auto const mask = GENERATE(0U, 0x63FU, 0x80000001U);
    auto observations = AlsaChannelMapObservations{.optCurrentPositions = legacySurroundPositions()};

    SECTION("a malformed matching offer remains terminal")
    {
      auto positions = legacySurroundPositions();
      positions[0] = SND_CHMAP_FR;
      observations.supportedMaps.push_back(std::move(positions));
    }

    SECTION("an empty offer remains terminal")
    {
      observations.supportedMaps.emplace_back();
    }

    SECTION("an oversized offer list remains terminal")
    {
      observations.supportedMaps.assign(257, legacySurroundPositions());
    }

    auto const mapRes = prepareAlsaChannelMap(
      surroundFormat(mask),
      [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(mapRes.error().error.code == Error::Code::InitFailed);
    CHECK(mapRes.error().error.message.contains("ALSA capability query returned a malformed channel map"));
  }

  TEST_CASE("AlsaChannelMap - source completeness precedes the legacy UAC1 ambiguity refusal",
            "[audio][unit][alsa-channel-map]")
  {
    auto const mask = GENERATE(0x1U, 0x80000001U);
    auto const mapRes =
      prepareAlsaChannelMap(surroundFormat(mask),
                            []
                            {
                              return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{
                                AlsaChannelMapObservations{.optCurrentPositions = legacySurroundPositions()}};
                            });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(mapRes.error().error.message ==
          "ALSA source channel mask is incomplete or contains unassigned speaker positions");
  }

  TEST_CASE("AlsaChannelMap - legacy UAC1 compatibility never swallows an independent reader error",
            "[audio][unit][alsa-channel-map]")
  {
    auto const mask = GENERATE(0U, 0x63FU, 0x80000001U);
    auto const mapRes = prepareAlsaChannelMap(
      surroundFormat(mask),
      []
      {
        return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{std::unexpected{
          BackendOpenFailure{.error = makeError(Error::Code::IoError, "independent channel-map reader fault").error(),
                             .cause = BackendOpenFailure::Cause::OutputFailure}}};
      });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(mapRes.error().error.code == Error::Code::IoError);
    CHECK(mapRes.error().error.message == "independent channel-map reader fault");
  }
} // namespace ao::audio::backend::detail::test
