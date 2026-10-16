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

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
    PcmFormat stereoFormat(std::uint32_t mask = 0x3)
    {
      return PcmFormat{
        .sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed24In32Le, .speakerMask = mask};
    }

    AlsaChannelMapObservations currentMap(std::vector<unsigned int> positions)
    {
      return AlsaChannelMapObservations{.supportedMaps = {}, .optCurrentPositions = std::move(positions)};
    }
  } // namespace

  TEST_CASE("AlsaChannelMap - independent NULL query and current observations preserve compatible delivery",
            "[audio][unit][alsa-channel-map]")
  {
    auto const channels = GENERATE(std::uint8_t{4}, std::uint8_t{6});
    auto const client = PcmFormat{.sampleRate = 48000,
                                  .channels = channels,
                                  .encoding = SampleEncoding::Signed24PackedLe,
                                  .speakerMask = channels == 4 ? 0x33U : 0x3fU};
    auto observations = AlsaChannelMapObservations{.supportedMaps = {}, .optCurrentPositions = {}};
    auto expectedQuery = AlsaChannelMapObservations::Query::Unavailable;
    bool expectsCurrent = false;

    SECTION("both calls return NULL")
    {
    }

    SECTION("NULL query does not suppress a valid current map")
    {
      observations.optCurrentPositions =
        channels == 4 ? std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_RL, SND_CHMAP_RR}
                      : std::vector<unsigned int>{
                          SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_RL, SND_CHMAP_RR};
      expectsCurrent = true;
    }

    SECTION("wrong-count capability is not current-position evidence")
    {
      observations.query = AlsaChannelMapObservations::Query::NoApplicableMap;
      observations.supportedMaps = {{SND_CHMAP_FL, SND_CHMAP_FR}};
      expectedQuery = AlsaChannelMapObservations::Query::NoApplicableMap;
    }

    SECTION("an applicable capability is still not a current map")
    {
      observations.query = AlsaChannelMapObservations::Query::NoApplicableMap;
      observations.supportedMaps =
        channels == 4 ? std::vector<std::vector<unsigned int>>{{SND_CHMAP_FR, SND_CHMAP_FL, SND_CHMAP_RR, SND_CHMAP_RL}}
                      : std::vector<std::vector<unsigned int>>{
                          {SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_RL, SND_CHMAP_RR}};
      expectedQuery = AlsaChannelMapObservations::Query::ApplicableMap;
    }

    std::size_t calls = 0;
    auto const mapRes =
      prepareAlsaChannelMap(client,
                            [&]
                            {
                              ++calls;
                              return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations};
                            });

    REQUIRE(mapRes);
    CHECK(calls == 1U);
    CHECK(mapRes->observations.query == expectedQuery);
    CHECK(mapRes->observations.optCurrentPositions.has_value() == expectsCurrent);
    CHECK(mapRes->delivery.mapping == (expectsCurrent ? ChannelDeliveryEvidence::Mapping::Mapped
                                                      : ChannelDeliveryEvidence::Mapping::LegacyUnverified));
    CHECK_FALSE(mapRes->delivery.hasSharedConversion);
    CHECK(mapRes->delivery.nativeSpeakerMask == (expectsCurrent ? client.speakerMask : 0U));
    CHECK_FALSE(mapRes->needsPermutation);
    CHECK(client.speakerMask == (channels == 4 ? 0x33U : 0x3fU));
  }

  TEST_CASE("AlsaChannelMap - reuse replans both identities from the cached actual observations",
            "[audio][unit][alsa-channel-map]")
  {
    // Component coverage for the open-time proof cached by AlsaExclusiveBackend:
    // both identities pass through this same active admission seam using cached
    // observations. The query itself never re-plans or performs native I/O;
    // native open/query controls separately check the cached result.

    auto const planFor = [](PcmFormat const& client, AlsaChannelMapObservations const& observations)
    {
      return prepareAlsaChannelMap(
        client,
        [&observations] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });
    };

    auto const sameTransport = [](AlsaChannelMap const& lhs, AlsaChannelMap const& rhs)
    {
      if (lhs.channels != rhs.channels || lhs.sampleBytes != rhs.sampleBytes ||
          lhs.needsPermutation != rhs.needsPermutation)
      {
        return false;
      }

      // needsPermutation false means identity; only the slots a plan actually
      // uses decide, never the unused array tail.
      for (std::size_t slot = 0; slot < lhs.channels; ++slot)
      {
        if (lhs.sourceSlots[slot] != rhs.sourceSlots[slot])
        {
          return false;
        }
      }

      return true;
    };

    SECTION("an identity FL/FR map shares one transport between 0 and 0x3")
    {
      auto const observations = currentMap({SND_CHMAP_FL, SND_CHMAP_FR});
      auto const identifiedRes = planFor(stereoFormat(0x3), observations);
      auto const unpositionedRes = planFor(stereoFormat(0), observations);
      REQUIRE(identifiedRes);
      REQUIRE(unpositionedRes);
      CHECK(sameTransport(*identifiedRes, *unpositionedRes));
      CHECK_FALSE(identifiedRes->needsPermutation);
      CHECK(identifiedRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
      CHECK(unpositionedRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
      CHECK(identifiedRes->delivery.nativeSpeakerMask == 0x3U);
      CHECK(unpositionedRes->delivery.nativeSpeakerMask == 0x3U);
    }

    SECTION("a missing current map leaves legacy reuse only while both plans agree")
    {
      auto const observations = AlsaChannelMapObservations{.supportedMaps = {}, .optCurrentPositions = {}};
      auto const identifiedRes = planFor(stereoFormat(0x3), observations);
      auto const unpositionedRes = planFor(stereoFormat(0), observations);
      REQUIRE(identifiedRes);
      REQUIRE(unpositionedRes);
      CHECK(sameTransport(*identifiedRes, *unpositionedRes));
      CHECK_FALSE(identifiedRes->needsPermutation);
      // No inferred native role or verification claim beyond the facts.
      CHECK(identifiedRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
      CHECK(unpositionedRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
      CHECK(identifiedRes->delivery.nativeSpeakerMask == 0U);
      CHECK(unpositionedRes->delivery.nativeSpeakerMask == 0U);
    }

    SECTION("a swapped map cannot gain 0 <-> 0x3 reuse")
    {
      auto const observations = currentMap({SND_CHMAP_FR, SND_CHMAP_FL});
      auto const identifiedRes = planFor(stereoFormat(0x3), observations);
      auto const unpositionedRes = planFor(stereoFormat(0), observations);
      REQUIRE(identifiedRes);
      REQUIRE(unpositionedRes);
      REQUIRE(identifiedRes->needsPermutation);
      CHECK(identifiedRes->sourceSlots[0] == 1U);
      CHECK(identifiedRes->sourceSlots[1] == 0U);
      CHECK_FALSE(unpositionedRes->needsPermutation);
      CHECK(unpositionedRes->sourceSlots[0] == 0U);
      CHECK(unpositionedRes->sourceSlots[1] == 1U);
      CHECK_FALSE(sameTransport(*identifiedRes, *unpositionedRes));
    }

    SECTION("a phase-tagged source-zero map cannot gain 0 <-> 0x3 reuse")
    {
      auto const observations = currentMap({SND_CHMAP_FL | SND_CHMAP_PHASE_INVERSE, SND_CHMAP_FR});
      auto const unpositionedRes = planFor(stereoFormat(0), observations);
      REQUIRE(unpositionedRes);
      CHECK(unpositionedRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
      CHECK(unpositionedRes->delivery.nativeSpeakerMask == 0U);
      CHECK_FALSE(planFor(stereoFormat(0x3), observations));
    }

    SECTION("conflicting current roles refuse the identified successor")
    {
      auto const observations = currentMap({SND_CHMAP_FC, SND_CHMAP_LFE});
      auto const unpositionedRes = planFor(stereoFormat(0), observations);
      REQUIRE(unpositionedRes);
      CHECK_FALSE(planFor(stereoFormat(0x3), observations));
    }
  }

  TEST_CASE("AlsaChannelMap - current same-set identity and permutation are both mapped",
            "[audio][unit][alsa-channel-map]")
  {
    bool const swapped = GENERATE(false, true);
    auto observations = currentMap(swapped ? std::vector<unsigned int>{SND_CHMAP_FR, SND_CHMAP_FL}
                                           : std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_FR});
    auto const mapRes = prepareAlsaChannelMap(
      stereoFormat(), [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });

    REQUIRE(mapRes);
    CHECK(mapRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0x3U);
    CHECK(mapRes->needsPermutation == swapped);
    CHECK(mapRes->sourceSlots[0] == (swapped ? 1U : 0U));
    CHECK(mapRes->sourceSlots[1] == (swapped ? 0U : 1U));
  }

  TEST_CASE("AlsaChannelMap - source zero never acquires guessed slot identity", "[audio][unit][alsa-channel-map]")
  {
    bool const silent = GENERATE(false, true);
    auto const positions = silent ? std::vector<unsigned int>{SND_CHMAP_NA, SND_CHMAP_NA}
                                  : std::vector<unsigned int>{SND_CHMAP_FR, SND_CHMAP_FL};
    auto const mapRes = prepareAlsaChannelMap(
      stereoFormat(0),
      [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{currentMap(positions)}; });

    REQUIRE(mapRes);
    REQUIRE(mapRes->observations.optCurrentPositions);
    CHECK(*mapRes->observations.optCurrentPositions == positions);
    CHECK(mapRes->delivery.nativeSpeakerMask == (silent ? 0U : 0x3U));
    CHECK(mapRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK_FALSE(mapRes->needsPermutation);
    CHECK(mapRes->sourceSlots[0] == 0U);
    CHECK(mapRes->sourceSlots[1] == 1U);
  }

  TEST_CASE("AlsaChannelMap - MONO is not an observed front-center identity", "[audio][unit][alsa-channel-map]")
  {
    auto const client =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x4};
    auto const mapRes = prepareAlsaChannelMap(
      client,
      [] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{currentMap({SND_CHMAP_MONO})}; });

    REQUIRE(mapRes);
    CHECK(mapRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0U);
    CHECK_FALSE(mapRes->needsPermutation);
  }

  TEST_CASE("AlsaChannelMap - known complete and partial contradictions remain contextual refusals",
            "[audio][unit][alsa-channel-map]")
  {
    auto observations = AlsaChannelMapObservations{};

    SECTION("same count different speaker set")
    {
      observations = currentMap({SND_CHMAP_FC, SND_CHMAP_LFE});
    }

    SECTION("partial map includes a conflicting known role")
    {
      observations = currentMap({SND_CHMAP_FC, SND_CHMAP_UNKNOWN});
    }

    SECTION("partial map cannot justify moving a known role")
    {
      observations = currentMap({SND_CHMAP_FR, SND_CHMAP_UNKNOWN});
    }

    SECTION("known but unrepresentable role is not unknown")
    {
      observations = currentMap({SND_CHMAP_FLW, SND_CHMAP_UNKNOWN});
    }

    SECTION("phase inversion is not a pure permutation")
    {
      observations = currentMap({SND_CHMAP_FL | SND_CHMAP_PHASE_INVERSE, SND_CHMAP_FR});
    }

    auto const mapRes = prepareAlsaChannelMap(
      stereoFormat(), [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(mapRes.error().error.code == Error::Code::FormatRejected);
    CHECK(mapRes.error().error.message.contains("ALSA current channel positions conflict"));
  }

  TEST_CASE("AlsaChannelMap - phase-flagged current refuses identified sources but keeps legacy playback",
            "[audio][unit][alsa-channel-map]")
  {
    auto const positions = std::vector<unsigned int>{SND_CHMAP_FL | SND_CHMAP_PHASE_INVERSE, SND_CHMAP_FR};

    SECTION("identified source keeps the contextual phase refusal")
    {
      auto const mapRes = prepareAlsaChannelMap(
        stereoFormat(),
        [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{currentMap(positions)}; });

      REQUIRE_FALSE(mapRes);
      CHECK(mapRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK(mapRes.error().error.code == Error::Code::FormatRejected);
      CHECK(mapRes.error().error.message.contains("ALSA current channel positions conflict"));
    }

    SECTION("unidentified source keeps no-permutation legacy playback")
    {
      auto const mapRes = prepareAlsaChannelMap(
        stereoFormat(0),
        [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{currentMap(positions)}; });

      REQUIRE(mapRes);
      REQUIRE(mapRes->observations.optCurrentPositions);
      CHECK(*mapRes->observations.optCurrentPositions == positions);
      CHECK(mapRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
      // The phase-flagged slot is not clean endpoint evidence; no native set
      // and no mapped polarity are claimed from the unidentified request.
      CHECK(mapRes->delivery.nativeSpeakerMask == 0U);
      CHECK_FALSE(mapRes->needsPermutation);
      CHECK(mapRes->sourceSlots[0] == 0U);
      CHECK(mapRes->sourceSlots[1] == 1U);
    }
  }

  TEST_CASE("AlsaChannelMap - silent current slots cannot carry identified source speakers",
            "[audio][unit][alsa-channel-map]")
  {
    bool const allSilent = GENERATE(false, true);
    auto const positions = allSilent ? std::vector<unsigned int>{SND_CHMAP_NA, SND_CHMAP_NA}
                                     : std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_NA};
    auto const mapRes = prepareAlsaChannelMap(
      stereoFormat(),
      [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{currentMap(positions)}; });

    // Repeated NA labels are a valid silent map, not malformed readback.
    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(mapRes.error().error.code == Error::Code::FormatRejected);
    CHECK(mapRes.error().error.message.contains("ALSA current channel positions conflict"));
  }

  TEST_CASE("AlsaChannelMap - silent capabilities never replace actual current evidence",
            "[audio][unit][alsa-channel-map]")
  {
    auto observations = AlsaChannelMapObservations{.query = AlsaChannelMapObservations::Query::ApplicableMap,
                                                   .supportedMaps = {{SND_CHMAP_NA, SND_CHMAP_NA}},
                                                   .optCurrentPositions = {}};
    auto expectedMapping = ChannelDeliveryEvidence::Mapping::LegacyUnverified;
    std::uint32_t expectedMask = 0;

    SECTION("missing current map retains uncertainty")
    {
      observations.optCurrentPositions.reset();
    }

    SECTION("a current mapped pair outranks the silent capability")
    {
      observations.optCurrentPositions = std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_FR};
      expectedMapping = ChannelDeliveryEvidence::Mapping::Mapped;
      expectedMask = 0x3;
    }

    auto const mapRes = prepareAlsaChannelMap(
      stereoFormat(), [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });
    REQUIRE(mapRes);
    CHECK(mapRes->observations.query == AlsaChannelMapObservations::Query::ApplicableMap);
    CHECK(mapRes->observations.supportedMaps == observations.supportedMaps);
    CHECK(mapRes->delivery.mapping == expectedMapping);
    CHECK(mapRes->delivery.nativeSpeakerMask == expectedMask);
    CHECK_FALSE(mapRes->needsPermutation);
  }

  TEST_CASE("AlsaChannelMap - non-applicable capability role defects cannot fault the applied current mode",
            "[audio][unit][alsa-channel-map]")
  {
    auto observations = AlsaChannelMapObservations{};
    observations.query = AlsaChannelMapObservations::Query::NoApplicableMap;
    observations.supportedMaps = {{SND_CHMAP_FL, SND_CHMAP_FL, SND_CHMAP_RL, SND_CHMAP_RR}};
    observations.optCurrentPositions = std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_FR};
    auto expectedQuery = AlsaChannelMapObservations::Query::NoApplicableMap;

    SECTION("a duplicate-role offer of another channel count is ignored")
    {
    }

    SECTION("a matching applicable capability is still discovered beside it")
    {
      observations.supportedMaps.push_back({SND_CHMAP_FR, SND_CHMAP_FL});
      expectedQuery = AlsaChannelMapObservations::Query::ApplicableMap;
    }

    auto const mapRes = prepareAlsaChannelMap(
      stereoFormat(), [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });

    REQUIRE(mapRes);
    CHECK(mapRes->observations.query == expectedQuery);
    CHECK(mapRes->observations.supportedMaps == observations.supportedMaps);
    CHECK(mapRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0x3U);
    CHECK_FALSE(mapRes->needsPermutation);
    CHECK(mapRes->sourceSlots[0] == 0U);
    CHECK(mapRes->sourceSlots[1] == 1U);
  }

  TEST_CASE("AlsaChannelMap - non-applicable capability storage bounds still fault the open",
            "[audio][unit][alsa-channel-map]")
  {
    auto observations = AlsaChannelMapObservations{};
    observations.query = AlsaChannelMapObservations::Query::NoApplicableMap;
    observations.optCurrentPositions = std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_FR};

    SECTION("oversized offer")
    {
      observations.supportedMaps = {std::vector<unsigned int>(kAlsaChannelCapacity + 1U, SND_CHMAP_FL)};
    }

    SECTION("empty offer")
    {
      observations.supportedMaps = {{}};
    }

    auto const mapRes = prepareAlsaChannelMap(
      stereoFormat(), [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(mapRes.error().error.code == Error::Code::InitFailed);
    CHECK(mapRes.error().error.message.contains("malformed channel map"));
  }

  TEST_CASE("AlsaChannelMap - partial compatible observations retain legacy uncertainty",
            "[audio][unit][alsa-channel-map]")
  {
    bool const allUnknown = GENERATE(false, true);
    auto const positions = allUnknown ? std::vector<unsigned int>{SND_CHMAP_UNKNOWN, SND_CHMAP_UNKNOWN}
                                      : std::vector<unsigned int>{SND_CHMAP_FL, SND_CHMAP_UNKNOWN};
    auto const mapRes = prepareAlsaChannelMap(
      stereoFormat(),
      [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{currentMap(positions)}; });

    REQUIRE(mapRes);
    CHECK(mapRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0U);
    CHECK_FALSE(mapRes->needsPermutation);
    REQUIRE(mapRes->observations.optCurrentPositions);
    CHECK(*mapRes->observations.optCurrentPositions == positions);
  }

  TEST_CASE("AlsaChannelMap - malformed successful maps are output failures", "[audio][unit][alsa-channel-map]")
  {
    auto observations = AlsaChannelMapObservations{};

    SECTION("wrong current count")
    {
      observations = currentMap({SND_CHMAP_FL});
    }

    SECTION("empty current map")
    {
      observations = currentMap({});
    }

    SECTION("duplicate known roles")
    {
      observations = currentMap({SND_CHMAP_FL, SND_CHMAP_FL});
    }

    SECTION("invalid role")
    {
      observations = currentMap({SND_CHMAP_FL, SND_CHMAP_LAST + 1U});
    }

    SECTION("invalid flags")
    {
      observations = currentMap({SND_CHMAP_FL, 0x80000000U | SND_CHMAP_FR});
    }

    SECTION("malformed capability cannot be hidden by current NULL")
    {
      observations.query = AlsaChannelMapObservations::Query::NoApplicableMap;
      observations.supportedMaps = {{SND_CHMAP_FL, SND_CHMAP_FL}};
    }

    auto const mapRes = prepareAlsaChannelMap(
      stereoFormat(), [&] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{observations}; });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    CHECK(mapRes.error().error.code == Error::Code::InitFailed);
    CHECK(mapRes.error().error.message.contains("malformed channel map"));
  }

  TEST_CASE("AlsaChannelMap - independent operation failure preserves its original error and cause",
            "[audio][unit][alsa-channel-map]")
  {
    auto const cause =
      GENERATE(BackendOpenFailure::Cause::OutputFailure, BackendOpenFailure::Cause::TrackRouteIncompatible);
    auto const failure = BackendOpenFailure{
      .error = makeError(Error::Code::IoError, "native observation stage: lost device").error(), .cause = cause};
    auto const mapRes = prepareAlsaChannelMap(stereoFormat(),
                                              [&] -> std::expected<AlsaChannelMapObservations, BackendOpenFailure>
                                              { return std::unexpected{failure}; });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().error.code == Error::Code::IoError);
    CHECK(mapRes.error().error.message == "native observation stage: lost device");
    CHECK(mapRes.error().cause == cause);
  }

  TEST_CASE("AlsaChannelMap - front-center mono cannot be relabelled as front-left", "[audio][unit][alsa-channel-map]")
  {
    auto const client =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x4};
    auto const mapRes = prepareAlsaChannelMap(
      client, [] { return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{currentMap({SND_CHMAP_FL})}; });

    REQUIRE_FALSE(mapRes);
    CHECK(mapRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(mapRes.error().error.code == Error::Code::FormatRejected);
  }

  TEST_CASE("AlsaChannelMap - six-channel current order drives the real render permutation",
            "[audio][unit][alsa-channel-map]")
  {
    auto const client =
      PcmFormat{.sampleRate = 48000, .channels = 6, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3f};
    auto const mapRes = prepareAlsaChannelMap(
      client,
      []
      {
        return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{
          currentMap({SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_RL, SND_CHMAP_RR, SND_CHMAP_FC, SND_CHMAP_LFE})};
      });
    REQUIRE(mapRes);
    REQUIRE(mapRes->needsPermutation);
    CHECK(mapRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
    CHECK(mapRes->delivery.nativeSpeakerMask == 0x3fU);
    // Canonical FL FR FC LFE BL BR -> observed FL FR BL BR FC LFE.
    auto bytes = std::to_array<std::uint8_t>({1, 0x80, 2, 0x7f, 3, 0x81, 4, 0x7e, 5, 0x82, 6, 0x7d});
    auto const expected = std::to_array<std::uint8_t>({1, 0x80, 2, 0x7f, 5, 0x82, 6, 0x7d, 3, 0x81, 4, 0x7e});
    auto scratch = std::array<std::byte, 12>{};

    applyAlsaChannelPermutation(std::as_writable_bytes(std::span<std::uint8_t>{bytes}), *mapRes, scratch);

    CHECK(bytes == expected);
  }

  TEST_CASE("AlsaChannelMap - production in-place permutation preserves literal sample bytes in every storage type",
            "[audio][unit][alsa-channel-map]")
  {
    struct SampleBytes final
    {
      SampleEncoding encoding;
      std::vector<std::uint8_t> canonical;
      std::vector<std::uint8_t> native;
    };

    // Two frames of independently labelled FL FR BL BR samples. The observed
    // native order is BL FL BR FR, a four-cycle, not a self-inverse swap.
    auto const fixtures = std::array{
      SampleBytes{SampleEncoding::Signed16Le,
                  {0x01, 0x80, 0x02, 0x7f, 0x03, 0x81, 0x04, 0x7e, 0x05, 0x82, 0x06, 0x7d, 0x07, 0x83, 0x08, 0x7c},
                  {0x03, 0x81, 0x01, 0x80, 0x04, 0x7e, 0x02, 0x7f, 0x07, 0x83, 0x05, 0x82, 0x08, 0x7c, 0x06, 0x7d}},
      SampleBytes{SampleEncoding::Signed24PackedLe,
                  {1, 0x11, 0x80, 2, 0x22, 0x7f, 3, 0x33, 0x81, 4, 0x44, 0x7e,
                   5, 0x55, 0x82, 6, 0x66, 0x7d, 7, 0x77, 0x83, 8, 0x88, 0x7c},
                  {3, 0x33, 0x81, 1, 0x11, 0x80, 4, 0x44, 0x7e, 2, 0x22, 0x7f,
                   7, 0x77, 0x83, 5, 0x55, 0x82, 8, 0x88, 0x7c, 6, 0x66, 0x7d}},
      SampleBytes{SampleEncoding::Signed24In32Le,
                  {1, 0x11, 0x80, 0xff, 2, 0x22, 0x7f, 0, 3, 0x33, 0x81, 0xff, 4, 0x44, 0x7e, 0,
                   5, 0x55, 0x82, 0xff, 6, 0x66, 0x7d, 0, 7, 0x77, 0x83, 0xff, 8, 0x88, 0x7c, 0},
                  {3, 0x33, 0x81, 0xff, 1, 0x11, 0x80, 0xff, 4, 0x44, 0x7e, 0, 2, 0x22, 0x7f, 0,
                   7, 0x77, 0x83, 0xff, 5, 0x55, 0x82, 0xff, 8, 0x88, 0x7c, 0, 6, 0x66, 0x7d, 0}},
      SampleBytes{SampleEncoding::Signed32Le,
                  {1, 0x11, 0x21, 0x80, 2, 0x22, 0x32, 0x7f, 3, 0x33, 0x43, 0x81, 4, 0x44, 0x54, 0x7e,
                   5, 0x55, 0x65, 0x82, 6, 0x66, 0x76, 0x7d, 7, 0x77, 0x87, 0x83, 8, 0x88, 0x98, 0x7c},
                  {3, 0x33, 0x43, 0x81, 1, 0x11, 0x21, 0x80, 4, 0x44, 0x54, 0x7e, 2, 0x22, 0x32, 0x7f,
                   7, 0x77, 0x87, 0x83, 5, 0x55, 0x65, 0x82, 8, 0x88, 0x98, 0x7c, 6, 0x66, 0x76, 0x7d}},
      SampleBytes{SampleEncoding::Float32Le,
                  {0, 0, 0x80, 0x3e, 0, 0, 0,    0xbf, 0, 0, 0x40, 0x3f, 0, 0, 0x80, 0xbf,
                   0, 0, 0,    0x3e, 0, 0, 0x80, 0xbe, 0, 0, 0,    0x3f, 0, 0, 0x40, 0xbf},
                  {0, 0, 0x40, 0x3f, 0, 0, 0x80, 0x3e, 0, 0, 0x80, 0xbf, 0, 0, 0,    0xbf,
                   0, 0, 0,    0x3f, 0, 0, 0,    0x3e, 0, 0, 0x40, 0xbf, 0, 0, 0x80, 0xbe}}};

    for (auto const& fixture : fixtures)
    {
      CAPTURE(fixture.encoding);
      auto const client =
        PcmFormat{.sampleRate = 48000, .channels = 4, .encoding = fixture.encoding, .speakerMask = 0x33};
      auto const mapRes =
        prepareAlsaChannelMap(client,
                              []
                              {
                                return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{
                                  currentMap({SND_CHMAP_RL, SND_CHMAP_FL, SND_CHMAP_RR, SND_CHMAP_FR})};
                              });
      REQUIRE(mapRes);
      REQUIRE(mapRes->needsPermutation);
      auto bytes = fixture.canonical;
      auto frameScratch = std::array<std::byte, kAlsaPermutationScratchBytes>{};

      applyAlsaChannelPermutation(std::as_writable_bytes(std::span<std::uint8_t>{bytes}), *mapRes, frameScratch);

      CHECK(bytes == fixture.native);

      auto const reverseRes =
        prepareAlsaChannelMap(client,
                              []
                              {
                                return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{
                                  currentMap({SND_CHMAP_FR, SND_CHMAP_RR, SND_CHMAP_FL, SND_CHMAP_RL})};
                              });
      REQUIRE(reverseRes);
      applyAlsaChannelPermutation(std::as_writable_bytes(std::span<std::uint8_t>{bytes}), *reverseRes, frameScratch);
      CHECK(bytes == fixture.canonical);

      auto const identityRes =
        prepareAlsaChannelMap(client,
                              []
                              {
                                return std::expected<AlsaChannelMapObservations, BackendOpenFailure>{
                                  currentMap({SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_RL, SND_CHMAP_RR})};
                              });
      REQUIRE(identityRes);
      applyAlsaChannelPermutation(std::as_writable_bytes(std::span<std::uint8_t>{bytes}), *identityRes, frameScratch);
      CHECK(bytes == fixture.canonical);
    }
  }
} // namespace ao::audio::backend::detail::test
