// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaChannelMap.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

extern "C"
{
#include <alsa/pcm.h>
}

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    constexpr char const* kKnownConflict =
      "ALSA current channel positions conflict with the source; no lossless slot mapping is available";
    constexpr char const* kIncompleteSource =
      "ALSA source channel mask is incomplete or contains unassigned speaker positions";
    constexpr char const* kMalformedCurrent = "ALSA current readback returned a malformed channel map";
    constexpr char const* kMalformedOffer = "ALSA capability query returned a malformed channel map";
    constexpr char const* kMalformedList = "ALSA capability query returned a malformed channel map list";

    PcmFormat signed16(std::size_t channels, std::uint32_t mask)
    {
      return PcmFormat{.sampleRate = 48000,
                       .channels = static_cast<std::uint8_t>(channels),
                       .encoding = SampleEncoding::Signed16Le,
                       .speakerMask = mask};
    }

    std::expected<AlsaChannelMap, BackendOpenFailure> readPrepared(PcmFormat const& format,
                                                                   AlsaChannelMapObservations observations,
                                                                   std::size_t& calls)
    {
      return prepareAlsaChannelMap(format,
                                   [&, observations]
                                   {
                                     ++calls;
                                     return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations};
                                   });
    }

    void expectIdentityBytes(AlsaChannelMap const& map)
    {
      auto pcm = std::vector<std::byte>(static_cast<std::size_t>(map.channels) * map.sampleBytes);

      for (std::size_t index = 0; index < pcm.size(); ++index)
      {
        pcm[index] = static_cast<std::byte>(index + 1U);
      }

      auto const expected = pcm;
      auto scratch = std::vector<std::byte>(pcm.size());
      applyAlsaChannelPermutation(pcm, map, scratch);
      CHECK(pcm == expected);
    }

    void expectOutputFailure(PcmFormat const& format, AlsaChannelMapObservations observations, char const* message)
    {
      std::size_t calls = 0;
      auto const mapRes = readPrepared(format, std::move(observations), calls);

      REQUIRE_FALSE(mapRes);
      CHECK(calls == 1U);
      CHECK(mapRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(mapRes.error().error.code == Error::Code::InitFailed);
      CHECK(mapRes.error().error.message == message);
    }

    void expectCurrentMalformed(std::vector<unsigned int> positions)
    {
      auto const format = signed16(positions.size(), 0U);
      expectOutputFailure(
        format, AlsaChannelMapObservations{.optCurrentPositions = std::move(positions)}, kMalformedCurrent);
    }

    void expectUncertainIdentity(AlsaChannelMap const& map)
    {
      CHECK_FALSE(map.delivery.isMapped);
      CHECK_FALSE(map.delivery.hasSharedConversion);
      CHECK(map.delivery.nativeSpeakerMask == 0U);
      CHECK_FALSE(map.needsPermutation);
      CHECK(map.sampleBytes == 2U);

      for (std::size_t slot = 0; slot < map.channels; ++slot)
      {
        CHECK(map.sourceSlots[slot] == static_cast<std::uint8_t>(slot));
      }

      expectIdentityBytes(map);
    }

    std::vector<unsigned int> legacyEight()
    {
      return {SND_CHMAP_FL,
              SND_CHMAP_FR,
              SND_CHMAP_FC,
              SND_CHMAP_LFE,
              SND_CHMAP_SL,
              SND_CHMAP_SR,
              SND_CHMAP_SL,
              SND_CHMAP_SR};
    }

    std::vector<unsigned int> oldTablePrefix(std::size_t count)
    {
      // Pinned old UAC1 table, used only to build fixture positions.
      auto const table = std::to_array<unsigned int>({SND_CHMAP_FL,
                                                      SND_CHMAP_FR,
                                                      SND_CHMAP_FC,
                                                      SND_CHMAP_LFE,
                                                      SND_CHMAP_SL,
                                                      SND_CHMAP_SR,
                                                      SND_CHMAP_FLC,
                                                      SND_CHMAP_FRC,
                                                      SND_CHMAP_RC,
                                                      SND_CHMAP_SL,
                                                      SND_CHMAP_SR,
                                                      SND_CHMAP_TC});
      return {table.begin(), table.begin() + static_cast<std::ptrdiff_t>(count)};
    }

    std::vector<unsigned int> padded(std::vector<unsigned int> positions, std::size_t unknownCount)
    {
      positions.insert(positions.end(), unknownCount, SND_CHMAP_UNKNOWN);
      return positions;
    }

    std::vector<unsigned int> ordinarySixteen()
    {
      return {SND_CHMAP_FL,
              SND_CHMAP_FR,
              SND_CHMAP_FC,
              SND_CHMAP_LFE,
              SND_CHMAP_RL,
              SND_CHMAP_RR,
              SND_CHMAP_FLC,
              SND_CHMAP_FRC,
              SND_CHMAP_RC,
              SND_CHMAP_SL,
              SND_CHMAP_SR,
              SND_CHMAP_TC,
              SND_CHMAP_TFL,
              SND_CHMAP_TFC,
              SND_CHMAP_TFR,
              SND_CHMAP_TRL};
    }
  } // namespace

  TEST_CASE("AlsaChannelMap - recognized repeated side positions keep uncertain identity transport",
            "[audio][unit][alsa-channel-map]")
  {
    auto accept = [](std::vector<unsigned int> positions, std::initializer_list<std::uint32_t> masks)
    {
      for (auto const mask : masks)
      {
        CAPTURE(mask, positions.size());
        std::size_t calls = 0;
        auto const observations = AlsaChannelMapObservations{.optCurrentPositions = positions};
        auto const mapRes = readPrepared(signed16(positions.size(), mask), observations, calls);

        REQUIRE(mapRes);
        CHECK(calls == 1U);
        CHECK(mapRes->channels == positions.size());
        CHECK(mapRes->observations.query == AlsaChannelMapObservations::Query::Unavailable);
        CHECK(mapRes->observations.supportedMaps.empty());
        REQUIRE(mapRes->observations.optCurrentPositions);
        CHECK(*mapRes->observations.optCurrentPositions == positions);
        expectUncertainIdentity(*mapRes);
      }
    };

    SECTION("six-channel config 0x633")
    {
      accept({SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_SL, SND_CHMAP_SR}, {0U, 0x633U});
    }

    SECTION("seven-channel config 0x637")
    {
      accept({SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_SL, SND_CHMAP_SR},
             {0U, 0x637U});
    }

    SECTION("eight-channel config 0x63f")
    {
      accept(legacyEight(), {0U, 0x63FU});
    }

    SECTION("nine-channel config 0x73f")
    {
      accept({SND_CHMAP_FL,
              SND_CHMAP_FR,
              SND_CHMAP_FC,
              SND_CHMAP_LFE,
              SND_CHMAP_SL,
              SND_CHMAP_SR,
              SND_CHMAP_RC,
              SND_CHMAP_SL,
              SND_CHMAP_SR},
             {0U, 0x73FU});
    }

    SECTION("absent-config prefixes of ten through twelve")
    {
      auto const rows =
        std::to_array<std::pair<std::uint32_t, std::uint32_t>>({{10U, 0x3FFU}, {11U, 0x7FFU}, {12U, 0xFFFU}});

      for (auto const& [count, mask] : rows)
      {
        CAPTURE(count);
        accept(oldTablePrefix(count), {0U, mask});
      }
    }

    SECTION("thirteen through fifteen accept height roles in trailing UNKNOWN")
    {
      auto const rows =
        std::to_array<std::pair<std::uint32_t, std::uint32_t>>({{13U, 0x1FFFU}, {14U, 0x3FFFU}, {15U, 0x7FFFU}});

      for (auto const& [count, mask] : rows)
      {
        CAPTURE(count);
        accept(padded(oldTablePrefix(12), count - 12U), {0U, mask});
      }
    }

    SECTION("padded nine accepts 0xe3f")
    {
      accept(padded(legacyEight(), 1), {0U, 0xE3FU});
    }

    SECTION("padded ten through fifteen accept a zero mask")
    {
      for (std::size_t count = 10; count <= 15; ++count)
      {
        CAPTURE(count);
        accept(padded(legacyEight(), count - 8U), {0U});
      }
    }

    SECTION("short seven config 0x23f")
    {
      accept({SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_SL},
             {0U, 0x23FU});
    }

    SECTION("repeated side-left keeps the later side-left")
    {
      // Old-table bits 4 and 9. 0x201 is FL|SL; 0x3 is refused separately.
      accept({SND_CHMAP_SL, SND_CHMAP_SL}, {0U, 0x201U});
    }

    SECTION("repeated side-right keeps the later side-right")
    {
      accept({SND_CHMAP_SR, SND_CHMAP_SR}, {0U, 0x401U});
    }

    SECTION("an earlier side-right is withheld when side-left repeats around it")
    {
      // 0x205 is FL|FC|SL. Keeping the middle SR would conflict with FC.
      accept({SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_SL}, {0U, 0x203U, 0x205U});
    }

    SECTION("a later side-right stays known after repeated side-left")
    {
      accept({SND_CHMAP_SL, SND_CHMAP_SL, SND_CHMAP_SR}, {0U, 0x601U});
    }
  }

  TEST_CASE("AlsaChannelMap - retained side roles still conflict with an incompatible complete source",
            "[audio][unit][alsa-channel-map]")
  {
    auto refuse = [](std::vector<unsigned int> positions, std::uint32_t mask)
    {
      CAPTURE(mask, positions.size());
      std::size_t calls = 0;
      auto const mapRes = readPrepared(
        signed16(positions.size(), mask), AlsaChannelMapObservations{.optCurrentPositions = positions}, calls);

      REQUIRE_FALSE(mapRes);
      CHECK(calls == 1U);
      CHECK(mapRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK(mapRes.error().error.code == Error::Code::FormatRejected);
      CHECK(mapRes.error().error.message == kKnownConflict);
    };

    SECTION("six-channel stable sides conflict with 0x3f")
    {
      refuse({SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_SL, SND_CHMAP_SR}, 0x3FU);
    }

    SECTION("exact eight conflicts with 0xff at the retained sides")
    {
      refuse(legacyEight(), 0xFFU);
    }

    SECTION("nonpadded nine conflicts with 0xe3f at stable back-center")
    {
      refuse({SND_CHMAP_FL,
              SND_CHMAP_FR,
              SND_CHMAP_FC,
              SND_CHMAP_LFE,
              SND_CHMAP_SL,
              SND_CHMAP_SR,
              SND_CHMAP_RC,
              SND_CHMAP_SL,
              SND_CHMAP_SR},
             0xE3FU);
    }

    SECTION("padded nine conflicts with 0x73f at stable side-left")
    {
      refuse(padded(legacyEight(), 1), 0x73FU);
    }

    SECTION("repeated side-left conflicts with stereo front roles")
    {
      refuse({SND_CHMAP_SL, SND_CHMAP_SL}, 0x3U);
    }

    SECTION("repeated side-right conflicts with stereo front roles")
    {
      refuse({SND_CHMAP_SR, SND_CHMAP_SR}, 0x3U);
    }

    SECTION("the later side-left between an early side-right stays known")
    {
      refuse({SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_SL}, 0x403U);
    }

    SECTION("the second side-left stays known")
    {
      // 0x501 is FL|RC|SR. Erasing every repeated side-left would accept it.
      refuse({SND_CHMAP_SL, SND_CHMAP_SL, SND_CHMAP_SR}, 0x501U);
    }

    SECTION("the later side-right stays known")
    {
      // 0xa01 is FL|SL|TC. Erasing that side-right would accept it.
      refuse({SND_CHMAP_SL, SND_CHMAP_SL, SND_CHMAP_SR}, 0xA01U);
    }
  }

  TEST_CASE("AlsaChannelMap - near-family signatures stay malformed channel maps", "[audio][unit][alsa-channel-map]")
  {
    SECTION("another duplicate identity is not erased")
    {
      expectCurrentMalformed({SND_CHMAP_FL, SND_CHMAP_FL});
    }

    SECTION("reordered front roles are not an old-table subsequence")
    {
      auto positions = legacyEight();
      std::swap(positions[0], positions[1]);
      expectCurrentMalformed(std::move(positions));
    }

    SECTION("side roles in an impossible old-table order stay malformed")
    {
      expectCurrentMalformed({SND_CHMAP_SL, SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_SR});
    }

    SECTION("interior UNKNOWN is not trailing padding")
    {
      expectCurrentMalformed({SND_CHMAP_FL,
                              SND_CHMAP_FR,
                              SND_CHMAP_FC,
                              SND_CHMAP_UNKNOWN,
                              SND_CHMAP_SL,
                              SND_CHMAP_SR,
                              SND_CHMAP_SL,
                              SND_CHMAP_SR});
    }

    SECTION("silent slots are not UNKNOWN padding")
    {
      expectCurrentMalformed({SND_CHMAP_SL, SND_CHMAP_SL, SND_CHMAP_NA});
    }

    SECTION("phase-flagged UNKNOWN is not bare padding")
    {
      auto positions = legacyEight();
      positions.push_back(SND_CHMAP_UNKNOWN | SND_CHMAP_PHASE_INVERSE);
      expectCurrentMalformed(std::move(positions));
    }

    SECTION("phase, driver, and reserved flags are not stripped")
    {
      auto phased = legacyEight();
      phased[6] |= SND_CHMAP_PHASE_INVERSE;
      expectCurrentMalformed(phased);

      auto driver = legacyEight();
      driver[4] |= SND_CHMAP_DRIVER_SPEC;
      expectCurrentMalformed(driver);

      auto reserved = legacyEight();
      reserved[0] |= 0x80000000U;
      expectCurrentMalformed(reserved);
    }

    SECTION("sixteen legacy-shaped slots exceed the historical bound")
    {
      expectCurrentMalformed(padded(oldTablePrefix(12), 4));
    }
  }

  TEST_CASE("AlsaChannelMap - current count mismatch stays malformed apart from a recognized shape",
            "[audio][unit][alsa-channel-map]")
  {
    auto positions = std::vector<unsigned int>{};

    SECTION("seven recognized positions do not satisfy an eight-channel mode")
    {
      positions = {SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_SL};
    }

    SECTION("nine recognized positions do not satisfy an eight-channel mode")
    {
      positions = padded(legacyEight(), 1);
    }

    expectOutputFailure(
      signed16(8, 0U), AlsaChannelMapObservations{.optCurrentPositions = std::move(positions)}, kMalformedCurrent);
  }

  TEST_CASE("AlsaChannelMap - incomplete or reserved sources stay layout refusals after a recognized map",
            "[audio][unit][alsa-channel-map]")
  {
    auto const mask = GENERATE(0x1U, 0x80000001U);
    auto positions = std::vector<unsigned int>{};

    SECTION("exact eight current")
    {
      positions = legacyEight();
    }

    SECTION("two-channel repeated side-left")
    {
      positions = {SND_CHMAP_SL, SND_CHMAP_SL};
    }

    std::size_t calls = 0;
    auto const mapRes = readPrepared(
      signed16(positions.size(), mask), AlsaChannelMapObservations{.optCurrentPositions = positions}, calls);

    REQUIRE_FALSE(mapRes);
    CHECK(calls == 1U);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(mapRes.error().error.code == Error::Code::FormatRejected);
    CHECK(mapRes.error().error.message == kIncompleteSource);
  }

  TEST_CASE("AlsaChannelMap - legacy offers do not select transport", "[audio][unit][alsa-channel-map]")
  {
    auto expectOfferOnly = [](std::vector<unsigned int> offer, std::uint32_t mask)
    {
      CAPTURE(mask, offer.size());
      std::size_t calls = 0;
      auto const observations = AlsaChannelMapObservations{
        .query = AlsaChannelMapObservations::Query::NoApplicableMap, .supportedMaps = {offer}};
      auto const mapRes = readPrepared(signed16(offer.size(), mask), observations, calls);

      REQUIRE(mapRes);
      CHECK(calls == 1U);
      CHECK(mapRes->observations.query == AlsaChannelMapObservations::Query::ApplicableMap);
      CHECK(mapRes->observations.supportedMaps == observations.supportedMaps);
      CHECK_FALSE(mapRes->observations.optCurrentPositions);
      expectUncertainIdentity(*mapRes);
    };

    SECTION("a compatible complete source is still not selected by the offer")
    {
      expectOfferOnly(legacyEight(), 0x63FU);
    }

    SECTION("a conflicting complete source is not refused without current positions")
    {
      expectOfferOnly(legacyEight(), 0xFFU);
    }

    SECTION("a two-channel ambiguous offer does not refuse stereo fronts")
    {
      expectOfferOnly({SND_CHMAP_SL, SND_CHMAP_SL}, 0x3U);
    }

    SECTION("a modern offer cannot replace ambiguous current positions")
    {
      std::size_t calls = 0;
      auto const observations = AlsaChannelMapObservations{.query = AlsaChannelMapObservations::Query::NoApplicableMap,
                                                           .supportedMaps = {{SND_CHMAP_FL,
                                                                              SND_CHMAP_FR,
                                                                              SND_CHMAP_FC,
                                                                              SND_CHMAP_LFE,
                                                                              SND_CHMAP_RL,
                                                                              SND_CHMAP_RR,
                                                                              SND_CHMAP_SL,
                                                                              SND_CHMAP_SR}},
                                                           .optCurrentPositions = legacyEight()};
      auto const mapRes = readPrepared(signed16(8, 0x63FU), observations, calls);

      REQUIRE(mapRes);
      CHECK(calls == 1U);
      CHECK(mapRes->observations.query == AlsaChannelMapObservations::Query::ApplicableMap);
      CHECK(mapRes->observations.supportedMaps == observations.supportedMaps);
      REQUIRE(mapRes->observations.optCurrentPositions);
      CHECK(*mapRes->observations.optCurrentPositions == legacyEight());
      expectUncertainIdentity(*mapRes);
    }
  }

  TEST_CASE("AlsaChannelMap - modern current positions permute independently of a legacy offer",
            "[audio][unit][alsa-channel-map]")
  {
    bool const permuted = GENERATE(false, true);
    auto positions = std::vector<unsigned int>{
      SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_RL, SND_CHMAP_RR, SND_CHMAP_SL, SND_CHMAP_SR};

    if (permuted)
    {
      std::swap(positions[0], positions[1]);
      std::swap(positions[4], positions[5]);
      std::swap(positions[6], positions[7]);
    }

    std::size_t calls = 0;
    auto const observations = AlsaChannelMapObservations{.query = AlsaChannelMapObservations::Query::NoApplicableMap,
                                                         .supportedMaps = {legacyEight()},
                                                         .optCurrentPositions = positions};
    auto const mapRes = readPrepared(signed16(8, 0x63FU), observations, calls);

    REQUIRE(mapRes);
    CHECK(calls == 1U);
    CHECK(mapRes->observations.query == AlsaChannelMapObservations::Query::ApplicableMap);
    CHECK(mapRes->observations.supportedMaps == observations.supportedMaps);
    REQUIRE(mapRes->observations.optCurrentPositions);
    CHECK(*mapRes->observations.optCurrentPositions == positions);
    CHECK(mapRes->delivery.isMapped);
    CHECK_FALSE(mapRes->delivery.hasSharedConversion);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0x63FU);
    CHECK(mapRes->needsPermutation == permuted);
    auto const expectedSlots = permuted ? std::to_array<std::uint8_t>({1, 0, 2, 3, 5, 4, 7, 6})
                                        : std::to_array<std::uint8_t>({0, 1, 2, 3, 4, 5, 6, 7});

    for (std::size_t slot = 0; slot < expectedSlots.size(); ++slot)
    {
      CHECK(mapRes->sourceSlots[slot] == expectedSlots[slot]);
    }

    auto bytes = std::to_array<std::uint8_t>(
      {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x05, 0x50, 0x06, 0x60, 0x07, 0x70, 0x08, 0x80});
    auto const expected =
      permuted ? std::to_array<std::uint8_t>(
                   {0x02, 0x20, 0x01, 0x10, 0x03, 0x30, 0x04, 0x40, 0x06, 0x60, 0x05, 0x50, 0x08, 0x80, 0x07, 0x70})
               : bytes;
    auto scratch = std::array<std::byte, 16>{};
    applyAlsaChannelPermutation(std::as_writable_bytes(std::span<std::uint8_t>{bytes}), *mapRes, scratch);
    CHECK(bytes == expected);
  }

  TEST_CASE("AlsaChannelMap - a nonduplicated side map stays an ordinary identity mapping",
            "[audio][unit][alsa-channel-map]")
  {
    auto const positions = std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_SL, SND_CHMAP_SR};
    std::size_t calls = 0;
    auto const mapRes =
      readPrepared(signed16(4, 0x603U), AlsaChannelMapObservations{.optCurrentPositions = positions}, calls);

    REQUIRE(mapRes);
    CHECK(calls == 1U);
    REQUIRE(mapRes->observations.optCurrentPositions);
    CHECK(*mapRes->observations.optCurrentPositions == positions);
    CHECK(mapRes->delivery.isMapped);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0x603U);
    CHECK_FALSE(mapRes->needsPermutation);
    CHECK(mapRes->sourceSlots[0] == 0U);
    CHECK(mapRes->sourceSlots[1] == 1U);
    CHECK(mapRes->sourceSlots[2] == 2U);
    CHECK(mapRes->sourceSlots[3] == 3U);
  }

  TEST_CASE("AlsaChannelMap - sixteen ordinary WAVE roles stay above the legacy storage bound",
            "[audio][unit][alsa-channel-map]")
  {
    auto const positions = ordinarySixteen();
    std::size_t calls = 0;
    auto const mapRes = readPrepared(
      signed16(positions.size(), 0xFFFFU), AlsaChannelMapObservations{.optCurrentPositions = positions}, calls);

    REQUIRE(mapRes);
    CHECK(calls == 1U);
    REQUIRE(mapRes->observations.optCurrentPositions);
    CHECK(*mapRes->observations.optCurrentPositions == positions);
    CHECK(mapRes->delivery.isMapped);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0xFFFFU);
    CHECK_FALSE(mapRes->needsPermutation);

    for (std::size_t slot = 0; slot < positions.size(); ++slot)
    {
      CHECK(mapRes->sourceSlots[slot] == static_cast<std::uint8_t>(slot));
    }
  }

  TEST_CASE("AlsaChannelMap - capability faults precede legacy admission", "[audio][unit][alsa-channel-map]")
  {
    SECTION("a duplicate-role offer remains terminal beside legacy current and an incomplete source")
    {
      auto offer = legacyEight();
      offer[1] = SND_CHMAP_FL;
      expectOutputFailure(signed16(8, 0x1U),
                          AlsaChannelMapObservations{.supportedMaps = {offer}, .optCurrentPositions = legacyEight()},
                          kMalformedOffer);
    }

    SECTION("an oversized offer list remains terminal beside legacy current")
    {
      auto offers = std::vector<std::vector<unsigned int>>(257, legacyEight());
      expectOutputFailure(
        signed16(8, 0U),
        AlsaChannelMapObservations{.supportedMaps = std::move(offers), .optCurrentPositions = legacyEight()},
        kMalformedList);
    }

    SECTION("a sixteen-slot legacy offer faults even beside ordinary current")
    {
      expectOutputFailure(signed16(16, 0U),
                          AlsaChannelMapObservations{
                            .supportedMaps = {padded(oldTablePrefix(12), 4)}, .optCurrentPositions = ordinarySixteen()},
                          kMalformedOffer);
    }
  }

  TEST_CASE("AlsaChannelMap - an independent reader error is not replaced by legacy admission",
            "[audio][unit][alsa-channel-map]")
  {
    auto const mask = GENERATE(0U, 0x63FU, 0x80000001U);
    auto const failure =
      BackendOpenFailure{.error = makeError(Error::Code::IoError, "independent channel-map reader fault").error(),
                         .cause = BackendOpenFailure::Cause::OutputFailure};
    std::size_t calls = 0;
    auto const mapRes = prepareAlsaChannelMap(signed16(8, mask),
                                              [&] -> std::expected<AlsaChannelMapObservations, BackendOpenFailure>
                                              {
                                                ++calls;
                                                return std::unexpected{failure};
                                              });

    REQUIRE_FALSE(mapRes);
    CHECK(calls == 1U);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(mapRes.error().error.code == Error::Code::IoError);
    CHECK(mapRes.error().error.message == "independent channel-map reader fault");
  }
} // namespace ao::audio::backend::detail::test
