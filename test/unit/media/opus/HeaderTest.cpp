// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/opus/Header.h>

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::opus::test
{
  namespace
  {
    struct HeadSpec final
    {
      std::string_view magic = "OpusHead";
      std::uint8_t version = 1;
      std::uint8_t channels = 2;
      std::uint16_t preSkip = 312;
      std::uint32_t inputSampleRate = 48000;
      std::int16_t outputGain = 0;
      std::uint8_t channelMappingFamily = kMappingFamilyMonoStereo;
      std::vector<std::uint8_t> mappingTail{};
    };

    std::vector<std::byte> makeHead(HeadSpec const& spec)
    {
      auto bytes = std::vector<std::uint8_t>{};

      for (char const character : spec.magic)
      {
        bytes.push_back(static_cast<std::uint8_t>(character));
      }

      bytes.push_back(spec.version);
      bytes.push_back(spec.channels);
      bytes.push_back(static_cast<std::uint8_t>(spec.preSkip & 0xFFU));
      bytes.push_back(static_cast<std::uint8_t>((spec.preSkip >> 8U) & 0xFFU));

      for (std::size_t index = 0; index < sizeof(spec.inputSampleRate); ++index)
      {
        bytes.push_back(static_cast<std::uint8_t>((spec.inputSampleRate >> (index * 8U)) & 0xFFU));
      }

      auto const gain = static_cast<std::uint16_t>(spec.outputGain);
      bytes.push_back(static_cast<std::uint8_t>(gain & 0xFFU));
      bytes.push_back(static_cast<std::uint8_t>((gain >> 8U) & 0xFFU));
      bytes.push_back(spec.channelMappingFamily);
      bytes.insert(bytes.end(), spec.mappingTail.begin(), spec.mappingTail.end());

      auto result = std::vector<std::byte>{};
      result.reserve(bytes.size());

      for (auto const byte : bytes)
      {
        result.push_back(static_cast<std::byte>(byte));
      }

      return result;
    }

    std::vector<std::byte> makePacket(std::string_view text)
    {
      auto result = std::vector<std::byte>{};
      result.reserve(text.size());

      for (char const character : text)
      {
        result.push_back(static_cast<std::byte>(character));
      }

      return result;
    }
  } // namespace

  TEST_CASE("parseHead derives an explicit stream layout for mapping family 0", "[media][unit][opus]")
  {
    SECTION("Stereo couples its two channels into one stream")
    {
      auto const headRes = parseHead(makeHead({}));
      REQUIRE(headRes);

      CHECK(headRes->channels == 2);
      CHECK(headRes->preSkip == 312);
      CHECK(headRes->inputSampleRate == 48000);
      CHECK(headRes->streamCount == 1);
      CHECK(headRes->coupledStreamCount == 1);
      CHECK(headRes->channelMapping[0] == 0);
      CHECK(headRes->channelMapping[1] == 1);
    }

    SECTION("Mono carries one uncoupled stream")
    {
      auto const headRes = parseHead(makeHead({.channels = 1}));
      REQUIRE(headRes);

      CHECK(headRes->channels == 1);
      CHECK(headRes->streamCount == 1);
      CHECK(headRes->coupledStreamCount == 0);
      CHECK(headRes->channelMapping[0] == 0);
    }

    SECTION("More than two channels is corrupt")
    {
      auto const headRes = parseHead(makeHead({.channels = 3}));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("parseHead reads an explicit channel mapping table", "[media][unit][opus]")
  {
    SECTION("Surround family maps every channel to a decoded stream channel")
    {
      auto const headRes = parseHead(
        makeHead({.channels = 3, .channelMappingFamily = kMappingFamilySurround, .mappingTail = {2, 1, 0, 1, 2}}));
      REQUIRE(headRes);

      CHECK(headRes->streamCount == 2);
      CHECK(headRes->coupledStreamCount == 1);
      CHECK(headRes->channelMapping[0] == 0);
      CHECK(headRes->channelMapping[1] == 1);
      CHECK(headRes->channelMapping[2] == 2);
    }

    SECTION("A silent channel index is accepted")
    {
      auto const headRes = parseHead(
        makeHead({.channels = 2, .channelMappingFamily = kMappingFamilyDiscrete, .mappingTail = {1, 0, 0, 255}}));
      REQUIRE(headRes);
      CHECK(headRes->channelMapping[1] == 255);
    }

    SECTION("A missing mapping table is corrupt")
    {
      auto const headRes =
        parseHead(makeHead({.channels = 3, .channelMappingFamily = kMappingFamilySurround, .mappingTail = {2, 1}}));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::CorruptData);
    }

    SECTION("More coupled streams than streams is corrupt")
    {
      auto const headRes = parseHead(
        makeHead({.channels = 2, .channelMappingFamily = kMappingFamilySurround, .mappingTail = {1, 2, 0, 1}}));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::CorruptData);
    }

    SECTION("A mapping index past the decoded stream channels is corrupt")
    {
      auto const headRes = parseHead(
        makeHead({.channels = 2, .channelMappingFamily = kMappingFamilySurround, .mappingTail = {1, 0, 0, 7}}));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::CorruptData);
    }

    SECTION("Family 1 beyond its eight Vorbis channel orders is corrupt")
    {
      auto const headRes = parseHead(makeHead({.channels = 9,
                                               .channelMappingFamily = kMappingFamilySurround,
                                               .mappingTail = {9, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}}));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::CorruptData);
    }

    SECTION("Family 1 at its eight channel limit is accepted")
    {
      auto const headRes = parseHead(makeHead({.channels = 8,
                                               .channelMappingFamily = kMappingFamilySurround,
                                               .mappingTail = {8, 0, 0, 1, 2, 3, 4, 5, 6, 7}}));
      REQUIRE(headRes);
      CHECK(headRes->channels == 8);
    }

    SECTION("Family 255 keeps carrying more than eight channels")
    {
      auto const headRes = parseHead(makeHead({.channels = 9,
                                               .channelMappingFamily = kMappingFamilyDiscrete,
                                               .mappingTail = {9, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}}));
      REQUIRE(headRes);
      CHECK(headRes->channels == 9);
      CHECK(headRes->streamCount == 9);
    }

    SECTION("A projection family this reader cannot describe is unsupported")
    {
      auto const headRes =
        parseHead(makeHead({.channels = 4, .channelMappingFamily = 2, .mappingTail = {2, 2, 0, 1, 2, 3}}));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::NotSupported);
    }
  }

  TEST_CASE("parseHead rejects packets it cannot identify", "[media][unit][opus][error]")
  {
    SECTION("A foreign magic signature is corrupt")
    {
      auto const headRes = parseHead(makeHead({.magic = "OpusTags"}));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::CorruptData);
    }

    SECTION("A packet shorter than the fixed header is corrupt")
    {
      auto const head = makeHead({});
      auto const headRes = parseHead(std::span{head}.first(12));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::CorruptData);
    }

    SECTION("An empty packet is corrupt")
    {
      auto const headRes = parseHead({});
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::CorruptData);
    }

    SECTION("Zero channels is corrupt")
    {
      auto const headRes = parseHead(makeHead({.channels = 0}));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("parseHead accepts every minor revision of the supported version", "[media][unit][opus]")
  {
    SECTION("A higher minor revision stays supported")
    {
      CHECK(parseHead(makeHead({.version = 15})));
    }

    SECTION("A higher major revision is not supported")
    {
      auto const headRes = parseHead(makeHead({.version = 16}));
      REQUIRE_FALSE(headRes);
      CHECK(headRes.error().code == Error::Code::NotSupported);
    }
  }

  TEST_CASE("parseTagsBody locates the comment list after the magic signature", "[media][unit][opus]")
  {
    SECTION("A tags packet exposes its body")
    {
      auto const packet = makePacket("OpusTagsBODY");
      auto const optBody = parseTagsBody(packet);
      REQUIRE(optBody);
      CHECK(optBody->size() == 4);
    }

    SECTION("A tags packet with no body exposes an empty span")
    {
      auto const packet = makePacket("OpusTags");
      auto const optBody = parseTagsBody(packet);
      REQUIRE(optBody);
      CHECK(optBody->empty());
    }

    SECTION("Another packet has no comment list")
    {
      CHECK_FALSE(parseTagsBody(makePacket("OpusHead")));
      CHECK_FALSE(parseTagsBody(makePacket("Opus")));
      CHECK_FALSE(parseTagsBody({}));
    }
  }
} // namespace ao::media::opus::test
