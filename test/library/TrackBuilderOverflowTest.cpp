// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/library/TrackRecordValidation.h"
#include "test/library/TrackBuilderTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("TrackBuilder - hot serialization rejects values that exceed header fields")
  {
    constexpr std::size_t kUint16Max = std::numeric_limits<std::uint16_t>::max();

    auto context = TrackSerializationFixture{};

    SECTION("Title length")
    {
      auto builder = TrackBuilder::makeEmpty();
      auto const title = std::string(kUint16Max + 1, 't');
      builder.metadata().title(title).artist("staged artist");

      auto const res = context.trySerializeHot(builder);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::ValueTooLarge);
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

      auto const res = context.trySerializeHot(builder);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::ValueTooLarge);
    }
  }

  TEST_CASE("TrackBuilder - cold serialization names the component that crosses the record limit")
  {
    auto context = TrackSerializationFixture{};
    auto builder = TrackBuilder::makeEmpty();
    builder.property().uri("track.flac");

    auto const checkRejectedAt = [&context, &builder](std::string_view const component)
    {
      auto const res = context.trySerializeCold(builder);
      REQUIRE_FALSE(res);
      INFO(res.error().message);
      CHECK(res.error().code == Error::Code::ValueTooLarge);
      CHECK(res.error().message.starts_with(component));
    };

    SECTION("A custom metadata value longer than the limit")
    {
      auto const value = std::string(kTrackColdRecordSizeLimit + 1, 'v');
      builder.customMetadata().add("key", value);
      checkRejectedAt("Custom metadata");
    }

    SECTION("Custom metadata values that cross the limit together")
    {
      constexpr std::size_t kValueCount = 2;
      constexpr std::size_t kValueSize =
        (kTrackColdRecordSizeLimit + 1 - sizeof(TrackColdHeader) - (kValueCount * sizeof(CustomMetadataEntry))) /
        kValueCount;

      auto const value = std::string(kValueSize, 'v');
      builder.customMetadata().add("first", value).add("second", value);
      checkRejectedAt("Custom metadata");
    }

    SECTION("A custom metadata entry table longer than the limit")
    {
      constexpr std::size_t kOverflowCount = (kTrackColdRecordSizeLimit / sizeof(CustomMetadataEntry)) + 1;

      auto keys = std::vector<std::string>{};
      keys.reserve(kOverflowCount);

      for (std::size_t i = 0; i < kOverflowCount; ++i)
      {
        keys.push_back("key" + std::to_string(i));
        builder.customMetadata().add(keys.back(), {});
      }

      checkRejectedAt("Custom metadata");
    }

    SECTION("Cover-art references longer than the limit")
    {
      constexpr std::size_t kOverflowCount = (kTrackColdRecordSizeLimit / sizeof(CoverArtEntry)) + 1;

      for (std::size_t i = 0; i < kOverflowCount; ++i)
      {
        builder.coverArt().add(PictureType::Other, ResourceId{static_cast<std::uint32_t>(i + 1)});
      }

      checkRejectedAt("Cover art references");
    }

    SECTION("A work block after cover-art references that nearly fill the record")
    {
      constexpr std::size_t kCoverCount = (kTrackColdRecordSizeLimit - sizeof(TrackColdHeader)) / sizeof(CoverArtEntry);
      static_assert(sizeof(TrackColdHeader) + (kCoverCount * sizeof(CoverArtEntry)) + sizeof(TrackWorkBlock) >
                    kTrackColdRecordSizeLimit);

      builder.metadata().movementNumber(1);

      for (std::size_t i = 0; i < kCoverCount; ++i)
      {
        builder.coverArt().add(PictureType::Other, ResourceId{static_cast<std::uint32_t>(i + 1)});
      }

      checkRejectedAt("Work");
    }
  }

  TEST_CASE("TrackBuilder - the cold record limit covers the whole record including padding")
  {
    // The limit is format: pin its value independently of the production constant.
    STATIC_REQUIRE(kTrackColdRecordSizeLimit == 65'535);
    // Records are padded to four bytes, so this is the largest stored record.
    constexpr auto kLargestRecordSize = kTrackColdRecordSizeLimit & ~std::size_t{3};
    constexpr auto kOneEntryCustomOverhead = sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry);
    constexpr auto kAlignedUri = std::string_view{"track01.flac"};
    constexpr auto kFillingValueSize =
      kLargestRecordSize - sizeof(TrackColdHeader) - kOneEntryCustomOverhead - kAlignedUri.size();
    static_assert(kAlignedUri.size() % 4 == 0 && kFillingValueSize % 4 == 0);

    auto context = TrackSerializationFixture{};
    auto const fillingValue = std::string(kFillingValueSize, 'v');
    auto builder = TrackBuilder::makeEmpty();
    builder.customMetadata().add("key", fillingValue);
    builder.property().uri(kAlignedUri);

    auto const checkRejectedAt = [&context, &builder](std::string_view const component)
    {
      auto const res = context.trySerializeCold(builder);
      REQUIRE_FALSE(res);
      INFO(res.error().message);
      CHECK(res.error().code == Error::Code::ValueTooLarge);
      CHECK(res.error().message.starts_with(component));
    };

    SECTION("A record of the largest size is accepted and admitted")
    {
      auto const res = context.trySerializeCold(builder);
      REQUIRE(res);
      CHECK(res->size() == 65'532);
      CHECK(validateSerializedColdTrack(*res));
    }

    SECTION("One more URI byte crosses the limit once padded")
    {
      builder.property().uri("track001.flac");
      checkRejectedAt("URI");
    }

    SECTION("A recording date after a fuller custom block crosses the limit at the performance block")
    {
      // Take the URI's room too, so the four-byte date-only performance block cannot fit.
      auto const fullerValue = fillingValue + std::string(kAlignedUri.size(), 'v');
      builder.customMetadata().clear().add("key", fullerValue);
      builder.metadata().recordingDate(RecordingDate{.year = 1999});
      checkRejectedAt("Recording date and credit references");
    }

    SECTION("A credit after a filled custom block crosses the limit at the performance block")
    {
      auto const credit = CreditView{.name = "Soloist", .kind = CreditKind::Soloist, .role = {}};
      builder.metadata().credits(std::span{&credit, 1});
      checkRejectedAt("Recording date and credit references");
    }
  }

  TEST_CASE("TrackBuilder - cold preflight reports invalid input before an oversized record")
  {
    auto context = TrackSerializationFixture{};
    auto builder = TrackBuilder::makeEmpty();
    builder.property().uri("track.flac");
    auto const oversizedValue = std::string(kTrackColdRecordSizeLimit + 1, 'v');
    builder.customMetadata().add("oversized", oversizedValue);

    auto const checkInvalidInput = [&context, &builder]
    {
      auto const res = context.trySerializeCold(builder);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
    };

    SECTION("A blank credit name")
    {
      auto const credit = CreditView{.name = " ", .kind = CreditKind::Soloist, .role = {}};
      builder.metadata().credits(std::span{&credit, 1});
      checkInvalidInput();
    }

    SECTION("A later custom metadata key that is not UTF-8")
    {
      builder.customMetadata().add("\xFF", "value");
      checkInvalidInput();
    }

    SECTION("A later custom metadata value that is not UTF-8")
    {
      builder.customMetadata().add("later", "\xFF");
      checkInvalidInput();
    }
  }

  TEST_CASE("TrackBuilder - cold serialization rejects durations outside the stored range")
  {
    auto context = TrackSerializationFixture{};

    for (auto const durationMs : std::array<std::int64_t, 5>{
           -1, -4294967296LL, 2147483648LL, 4294967295LL, std::numeric_limits<std::int64_t>::max()})
    {
      CAPTURE(durationMs);
      auto builder = TrackBuilder::makeEmpty();
      builder.property().uri("duration.flac").duration(std::chrono::milliseconds{durationMs});
      auto const res = context.trySerializeCold(builder);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == (durationMs < 0 ? Error::Code::InvalidInput : Error::Code::ValueTooLarge));
      CHECK(res.error().message.contains("Track duration"));
    }
  }

  TEST_CASE("TrackBuilder - complete preflight keeps rejected cold data from staging shared dictionary rows")
  {
    constexpr auto kUint16Overflow = static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1U;
    auto context = TrackSerializationFixture{};
    auto oversizedValue = std::string(kUint16Overflow, 'v');
    auto rejected = TrackBuilder::makeEmpty();
    rejected.metadata().artist("rejected-artist");
    rejected.property().uri("rejected.flac");

    SECTION("Oversized custom metadata")
    {
      rejected.customMetadata().add("oversized-key", oversizedValue);
    }

    SECTION("Duration outside the stored range")
    {
      rejected.customMetadata().add("oversized-key", "must not be staged");
      rejected.property().duration(std::chrono::milliseconds{2147483648LL});
    }

    auto rejectedRes = physicalSerializeTrack(rejected, context.transaction(), context.resources());
    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);

    auto accepted = TrackBuilder::makeEmpty();
    accepted.metadata().artist("accepted-artist");
    accepted.property().uri("accepted.flac");
    auto acceptedRes = physicalSerializeTrack(accepted, context.transaction(), context.resources());
    REQUIRE(acceptedRes);
    REQUIRE(context.transaction().commit());

    CHECK_FALSE(context.dictionary().findId("rejected-artist"));
    CHECK_FALSE(context.dictionary().findId("oversized-key"));
    CHECK(context.dictionary().findId("accepted-artist"));
  }
} // namespace ao::library::test
