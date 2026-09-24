// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackBuilderTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
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
    builder.property().uri("test.flac");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 0);
    CHECK(header->tagBloom == 0);
  }

  TEST_CASE("TrackBuilder - serializes multiple tags", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Test");
    builder.property().uri("test.flac");
    builder.tags().add("tag1").add("tag2").add("tag3");

    auto context = TrackSerializationFixture{};
    auto const [hotData, coldData] = context.serialize(builder);
    auto const view = TrackView{hotData, coldData};

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 12); // 3 tags * 4 bytes each
    CHECK(header->tagBloom == 0x0000000EU);
    REQUIRE(view.tags().count() == 3);
    CHECK(view.tags().id(0) == DictionaryId{1});
    CHECK(view.tags().id(1) == DictionaryId{2});
    CHECK(view.tags().id(2) == DictionaryId{3});
    CHECK(context.dictionary().get(view.tags().id(0)) == "tag1");
    CHECK(context.dictionary().get(view.tags().id(1)) == "tag2");
    CHECK(context.dictionary().get(view.tags().id(2)) == "tag3");
  }

  TEST_CASE("TrackBuilder - serializes one tag", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Test");
    builder.property().uri("test.flac");
    builder.tags().add("tag42");

    auto context = TrackSerializationFixture{};
    auto const [hotData, coldData] = context.serialize(builder);
    auto const view = TrackView{hotData, coldData};

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 4); // 1 tag * 4 bytes
    CHECK(header->tagBloom == 0x00000002U);
    REQUIRE(view.tags().count() == 1);
    CHECK(view.tags().id(0) == DictionaryId{1});
    CHECK(context.dictionary().get(view.tags().id(0)) == "tag42");
  }

  TEST_CASE("TrackBuilder - computes tag bloom filters with tags", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Test");
    builder.property().uri("test.flac");
    builder.tags().add("tag1").add("tag2").add("tag3").add("tag4").add("tag5");

    auto context = TrackSerializationFixture{};
    auto const [hotData, coldData] = context.serialize(builder);
    auto const view = TrackView{hotData, coldData};

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 20); // 5 tags * 4 bytes each
    CHECK(header->tagBloom == 0x0000003EU);
    REQUIRE(view.tags().count() == 5);
    constexpr auto kExpectedIds = std::to_array<DictionaryId>(
      {DictionaryId{1}, DictionaryId{2}, DictionaryId{3}, DictionaryId{4}, DictionaryId{5}});
    constexpr auto kExpectedNames = std::to_array<std::string_view>({"tag1", "tag2", "tag3", "tag4", "tag5"});

    for (std::uint16_t index = 0; index < view.tags().count(); ++index)
    {
      CHECK(view.tags().id(index) == kExpectedIds[index]);
      CHECK(context.dictionary().get(kExpectedIds[index]) == kExpectedNames[index]);
    }
  }

  TEST_CASE("TrackBuilder - serializeHot writes tag header data", "[library][unit][track-builder][tag]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata().title("Test Title");
    builder.property().uri("/path/to/file.flac");
    builder.tags().add("tag10").add("tag20");

    auto context = TrackSerializationFixture{};
    auto hotDataRes = physicalSerializeHotTrack(builder, context.transaction());
    REQUIRE(hotDataRes);
    auto const& hotData = *hotDataRes;

    REQUIRE(context.transaction().commit());
    auto const view = TrackView{hotData, std::span<std::byte const>{}};
    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->tagLength == 8); // 2 tags * 4 bytes
    CHECK(header->tagBloom == 0x00000006U);
    REQUIRE(view.tags().count() == 2);
    CHECK(view.tags().id(0) == DictionaryId{1});
    CHECK(view.tags().id(1) == DictionaryId{2});
    CHECK(context.dictionary().get(view.tags().id(0)) == "tag10");
    CHECK(context.dictionary().get(view.tags().id(1)) == "tag20");
  }
} // namespace ao::library::test
