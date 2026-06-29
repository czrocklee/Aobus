// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackBuilderTestSupport.h"
#include <ao/Type.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>

namespace ao::library::test
{
  TEST_CASE("TrackBuilder - serializes empty builders", "[library][unit][track][builder][serialization]")
  {
    auto builder = TrackBuilder::createNew();
    auto const [hotData, coldData] = serializeTestTrack(builder);

    CHECK(hotData.size() >= sizeof(TrackHotHeader));
    CHECK(!hotData.empty());
  }

  TEST_CASE("TrackBuilder - serializes strings into hot payloads", "[library][unit][track][builder][serialization]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Hello World").year(2021);
    builder.property().uri("/music/test.flac");

    auto const [hotData, coldData] = serializeTestTrack(builder);

    CHECK(hotData.size() >= sizeof(TrackHotHeader));

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());

    CHECK(header->titleLength == 11); // "Hello World"
    CHECK(header->year == 2021);

    auto const* payloadStart = reinterpret_cast<char const*>(hotData.data()) + sizeof(TrackHotHeader);
    CHECK(std::strncmp(payloadStart, "Hello World", 11) == 0);
  }

  TEST_CASE("TrackBuilder - serialize writes hot header fields", "[library][unit][track][builder][serialization]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().year(1999);
    builder.property().bitDepth(BitDepth{24});

    auto const [hotData, coldData] = serializeTestTrack(builder);
    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());

    CHECK(header->year == 1999);
    CHECK(header->bitDepth == 24);
  }

  TEST_CASE("TrackBuilder - serializes strings with special characters",
            "[library][unit][track][builder][serialization]")
  {
    auto builder = TrackBuilder::createNew();
    auto const* title = "Test: \"Quotes\" & 'Apostrophes'";
    builder.metadata().title(title);

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackHotHeader const*>(hotData.data());
    CHECK(header->titleLength == std::strlen(title));
  }

  TEST_CASE("TrackBuilder - serialization is stable across repeated calls",
            "[library][unit][track][builder][serialization]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().title("Test");
    builder.property().uri("/test");

    auto const [hotData1, coldData1] = serializeTestTrack(builder);
    auto const [hotData2, coldData2] = serializeTestTrack(builder);

    CHECK(hotData1.size() == hotData2.size());
    CHECK(hotData1 == hotData2);
  }

  TEST_CASE("TrackBuilder - serialize writes cold header fields", "[library][unit][track][builder][serialization]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().trackNumber(5).trackTotal(10).discNumber(1).discTotal(2);
    builder.property().uri("/path/to/file.flac").duration(std::chrono::minutes{3});

    auto const [hotData, coldData] = serializeTestTrack(builder);

    auto const* header = reinterpret_cast<TrackColdHeader const*>(coldData.data());
    CHECK(header->duration == std::chrono::minutes{3});
    CHECK(header->trackNumber == 5);
    CHECK(header->trackTotal == 10);
    CHECK(header->discNumber == 1);
    CHECK(header->discTotal == 2);
  }

  TEST_CASE("TrackBuilder - serializeCold writes cold view data", "[library][unit][track][builder][serialization]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata().trackNumber(3);
    builder.property().uri("/path/to/file.flac").duration(std::chrono::minutes{4});
    builder.customMetadata().add("key1", "value1").add("key2", "value2");

    auto context = TrackSerializationContext{};
    auto const coldData = context.serializeCold(builder);

    auto view = TrackView{std::span<std::byte const>{}, coldData};
    CHECK(view.property().duration() == std::chrono::minutes{4});
    CHECK(view.metadata().trackNumber() == 3);
  }

  TEST_CASE("TrackBuilder - fromView reconstructs builder fields", "[library][unit][track][builder][serialization]")
  {
    auto context = TrackSerializationContext{};

    auto original = TrackBuilder::createNew();
    original.metadata().title("Title").albumArtist("Test Album Artist").composer("Test Composer");
    original.property().uri("/path.flac");

    auto const [hotData, coldData] = context.serialize(original);
    auto view = TrackView{hotData, coldData};

    auto reconstructed = TrackBuilder::fromView(view, context.dict());
    CHECK(reconstructed.metadata().title() == "Title");
    CHECK(reconstructed.metadata().albumArtist() == "Test Album Artist");
    CHECK(reconstructed.metadata().composer() == "Test Composer");
    CHECK(reconstructed.property().uri() == "/path.flac");

    auto const& constBuilder = reconstructed;
    CHECK(constBuilder.property().uri() == "/path.flac");
    CHECK(constBuilder.tags().names().empty());
  }

  TEST_CASE("TrackBuilder - serialized views expose property and metadata fields",
            "[library][unit][track][builder][serialization]")
  {
    auto context = TrackSerializationContext{};

    auto builder = TrackBuilder::createNew();
    builder.metadata()
      .trackNumber(1)
      .trackTotal(10)
      .discNumber(2)
      .discTotal(3)
      .album("Album")
      .genre("Genre")
      .albumArtist("Album Artist");
    builder.coverArt().add(PictureType::FrontCover, ResourceId{42});
    builder.tags().add("tag1").add("tag2");

    auto const [hotData, coldData] = context.serialize(builder);
    auto view = TrackView{hotData, coldData};

    CHECK(view.metadata().trackNumber() == 1);
    CHECK(view.metadata().trackTotal() == 10);
    CHECK(view.metadata().discNumber() == 2);
    CHECK(view.metadata().discTotal() == 3);
    REQUIRE(view.coverArt().primary());
    CHECK(view.coverArt().primary()->resourceId == ResourceId{42});
    CHECK(view.metadata().albumId() == context.dict().getId("Album"));
    CHECK(view.metadata().genreId() == context.dict().getId("Genre"));
    CHECK(view.metadata().albumArtistId() == context.dict().getId("Album Artist"));
  }
} // namespace ao::library::test
