// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackBuilderTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("TrackBuilder - hot serialization rejects values that exceed header fields",
            "[library][unit][track-builder][overflow]")
  {
    constexpr std::size_t kUint16Max = std::numeric_limits<std::uint16_t>::max();

    auto context = TrackSerializationFixture{};

    SECTION("Title length")
    {
      auto builder = TrackBuilder::makeEmpty();
      auto const title = std::string(kUint16Max + 1, 't');
      builder.metadata().title(title).artist("staged artist");

      auto const result = context.trySerializeHot(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
      CHECK_FALSE(context.dictionary().findId("staged artist"));
    }

    SECTION("Tag payload length")
    {
      constexpr std::size_t kOverflowTagCount = (kUint16Max / sizeof(DictionaryId)) + 1;

      auto tagNames = std::vector<std::string>{};
      tagNames.reserve(kOverflowTagCount);

      for (std::size_t i = 0; i < kOverflowTagCount; ++i)
      {
        tagNames.push_back("tag" + std::to_string(i));
      }

      auto builder = TrackBuilder::makeEmpty();

      for (auto const& tagName : tagNames)
      {
        builder.tags().add(tagName);
      }

      auto const result = context.trySerializeHot(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }
  }

  TEST_CASE("TrackBuilder - cold serialization rejects values that exceed header fields",
            "[library][unit][track-builder][overflow]")
  {
    constexpr std::size_t kUint16Max = std::numeric_limits<std::uint16_t>::max();
    constexpr auto kUint16Overflow = kUint16Max + 1;

    auto context = TrackSerializationFixture{};

    SECTION("URI length")
    {
      auto builder = TrackBuilder::makeEmpty();
      auto const uri = std::string(kUint16Overflow, 'u');
      builder.property().uri(uri);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Custom metadata value length")
    {
      auto builder = TrackBuilder::makeEmpty();
      auto const value = std::string(kUint16Overflow, 'v');
      builder.customMetadata().add("key", value);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Cover art payload length")
    {
      constexpr std::size_t kOverflowCount = (kUint16Max / sizeof(CoverArtEntry)) + 1;

      auto builder = TrackBuilder::makeEmpty();

      for (std::size_t i = 0; i < kOverflowCount; ++i)
      {
        builder.coverArt().add(PictureType::Other, ResourceId{static_cast<std::uint32_t>(i + 1)});
      }

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Custom metadata count")
    {
      auto builder = TrackBuilder::makeEmpty();

      for (std::size_t i = 0; i < kUint16Overflow; ++i)
      {
        builder.customMetadata().add("key", {});
      }

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Custom metadata table offset")
    {
      constexpr std::size_t kOverflowCount = (kUint16Max / sizeof(CustomMetadataEntry)) + 1;

      auto builder = TrackBuilder::makeEmpty();

      for (std::size_t i = 0; i < kOverflowCount; ++i)
      {
        builder.customMetadata().add("key", {});
      }

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("URI offset after aligned custom payload")
    {
      constexpr std::size_t kValueSize = kUint16Max - sizeof(CustomMetadataBlockHeader) - sizeof(CustomMetadataEntry);

      auto builder = TrackBuilder::makeEmpty();
      auto const value = std::string(kValueSize, 'v');
      builder.customMetadata().add("key", value);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("URI offset after accumulated custom metadata values")
    {
      constexpr std::size_t kValueCount = 2;
      constexpr std::size_t kValueSize =
        (kUint16Overflow - sizeof(TrackColdHeader) - (kValueCount * sizeof(CustomMetadataEntry))) / kValueCount;

      auto builder = TrackBuilder::makeEmpty();
      auto const value = std::string(kValueSize, 'v');

      builder.customMetadata().add("first", value).add("second", value);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("URI offset after multiple aligned payloads")
    {
      constexpr std::size_t kCoverCount = kUint16Max / sizeof(CoverArtEntry);

      auto builder = TrackBuilder::makeEmpty();
      builder.metadata().movementNumber(1);

      for (std::size_t i = 0; i < kCoverCount; ++i)
      {
        builder.coverArt().add(PictureType::Other, ResourceId{static_cast<std::uint32_t>(i + 1)});
      }

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }
  }
} // namespace ao::library::test
