// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackBuilderTestSupport.h"
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

namespace ao::library::test
{
  TEST_CASE("TrackBuilder - tags builder adds removes and clears names", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();

    builder.tags().add("rock");
    builder.tags().add("jazz");
    builder.tags().add("blues");

    CHECK(builder.tags().names().size() == 3);

    builder.tags().remove("jazz");
    CHECK(builder.tags().names().size() == 2);

    CHECK(std::ranges::contains(builder.tags().names(), std::string_view{"rock"}));
    CHECK(std::ranges::contains(builder.tags().names(), std::string_view{"blues"}));

    builder.tags().clear();
    CHECK(builder.tags().names().empty());
  }

  TEST_CASE("TrackBuilder - serializes empty tag data", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Test");
    builder.property().uri("/test");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 0);
    CHECK(header->tagBloom == 0);
  }

  TEST_CASE("TrackBuilder - serializes multiple tags", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Test");
    builder.property().uri("/test");
    builder.tags().add("tag1").add("tag2").add("tag3");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 12); // 3 tags * 4 bytes each
    CHECK(header->tagBloom != 0);   // Bloom should be computed from tag IDs
  }

  TEST_CASE("TrackBuilder - serializes one tag", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Test");
    builder.property().uri("/test");
    builder.tags().add("tag42");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 4); // 1 tag * 4 bytes
  }

  TEST_CASE("TrackBuilder - computes tag bloom filters with tags", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Test");
    builder.property().uri("/test");
    builder.tags().add("tag1").add("tag2").add("tag3").add("tag4").add("tag5");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 20); // 5 tags * 4 bytes each
    CHECK(header->tagBloom != 0);
  }

  TEST_CASE("TrackBuilder - serializeHot writes tag header data", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Test Title");
    builder.property().uri("/path/to/file.flac");
    builder.tags().add("tag10").add("tag20");

    auto context = TrackSerializationFixture{};
    auto hotDataResult = builder.serializeHot(context.transaction());
    REQUIRE(hotDataResult);
    auto const& hotData = *hotDataResult;

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 8); // 2 tags * 4 bytes

    CHECK(header->tagBloom != 0);
  }
} // namespace ao::library::test
