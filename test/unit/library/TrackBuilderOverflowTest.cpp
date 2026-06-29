// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackBuilderTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace ao::library::test
{
  TEST_CASE("TrackBuilder - cold serialization rejects values that exceed header fields",
            "[library][unit][track][builder][serialization][overflow][error]")
  {
    constexpr std::size_t kUint16Max = std::numeric_limits<std::uint16_t>::max();
    constexpr auto kUint16Overflow = kUint16Max + 1;

    auto context = TrackSerializationContext{};

    SECTION("URI length")
    {
      auto builder = TrackBuilder::createNew();
      auto const uri = std::string(kUint16Overflow, 'u');
      builder.property().uri(uri);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Custom metadata value length")
    {
      auto builder = TrackBuilder::createNew();
      auto const value = std::string(kUint16Overflow, 'v');
      builder.customMetadata().add("key", value);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Cover art count")
    {
      auto builder = TrackBuilder::createNew();

      for (std::size_t i = 0; i < kUint16Overflow; ++i)
      {
        builder.coverArt().add(PictureType::Other, ResourceId{static_cast<std::uint32_t>(i + 1)});
      }

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Custom metadata count")
    {
      auto builder = TrackBuilder::createNew();

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
      constexpr std::size_t kOverflowCount = (kUint16Overflow / sizeof(CoverArtEntry));

      auto builder = TrackBuilder::createNew();

      for (std::size_t i = 0; i < kOverflowCount; ++i)
      {
        builder.coverArt().add(PictureType::Other, ResourceId{static_cast<std::uint32_t>(i + 1)});
      }

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("URI offset after accumulated custom metadata values")
    {
      constexpr std::size_t kValueCount = 2;
      constexpr std::size_t kValueSize =
        (kUint16Overflow - sizeof(TrackColdHeader) - (kValueCount * sizeof(CustomMetadataEntry))) / kValueCount;

      auto builder = TrackBuilder::createNew();
      auto const value = std::string(kValueSize, 'v');

      builder.customMetadata().add("first", value).add("second", value);

      auto const result = context.trySerializeCold(builder);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::ValueTooLarge);
    }
  }
} // namespace ao::library::test
