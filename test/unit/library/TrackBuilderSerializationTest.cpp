// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackBuilderTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    TrackColdHeader const& coldHeader(std::span<std::byte const> coldData)
    {
      auto const* header = utility::layout::view<TrackColdHeader>(coldData);
      REQUIRE(header != nullptr);
      return *header;
    }

    std::uint16_t coldBlockOffset(TrackColdHeader const& header, TrackColdBlockSlot slot)
    {
      return header.blockOffsets[trackColdBlockSlotIndex(slot)];
    }

    constexpr std::size_t alignToWord(std::size_t size) noexcept
    {
      return ((size + 3U) / 4U) * 4U;
    }

    std::vector<TrackColdBlockSlot> coldBlockSlots(std::span<std::byte const> coldData)
    {
      auto const reader = detail::TrackColdReader{coldData};
      REQUIRE(reader.valid());

      auto const& header = reader.header();
      auto result = std::vector<TrackColdBlockSlot>{};

      for (auto const slot :
           {TrackColdBlockSlot::CoverArt, TrackColdBlockSlot::Classical, TrackColdBlockSlot::CustomMetadata})
      {
        if (coldBlockOffset(header, slot) != 0)
        {
          result.push_back(slot);
        }
      }

      return result;
    }
  } // namespace

  TEST_CASE("TrackBuilder - serializes empty builders", "[library][unit][track][builder][serialization]")
  {
    auto builder = TrackBuilder::createNew();
    auto const [hotData, coldData] = serializeTestTrack(builder);

    CHECK(hotData.size() >= sizeof(TrackHotHeader));
    CHECK(!hotData.empty());
  }

  TEST_CASE("TrackBuilder - serializes cold records without extension blocks",
            "[library][unit][track][builder][serialization]")
  {
    auto context = TrackSerializationContext{};

    SECTION("empty URI")
    {
      auto builder = TrackBuilder::createNew();
      auto const coldData = context.serializeCold(builder);
      auto const& header = coldHeader(coldData);

      CHECK(coldData.size() == sizeof(TrackColdHeader));
      CHECK(header.blockOffsets == std::array<std::uint16_t, kTrackColdBlockSlotCount>{});
      CHECK(header.uriOffset == sizeof(TrackColdHeader));
      CHECK(header.uriLength == 0);
      CHECK(detail::TrackColdReader{coldData}.valid());
    }

    SECTION("non-empty URI")
    {
      auto builder = TrackBuilder::createNew();
      builder.property().uri("abc");

      auto const coldData = context.serializeCold(builder);
      auto const& header = coldHeader(coldData);
      auto const view = TrackView{std::span<std::byte const>{}, coldData};

      CHECK(coldData.size() == sizeof(TrackColdHeader) + 4);
      CHECK(header.blockOffsets == std::array<std::uint16_t, kTrackColdBlockSlotCount>{});
      CHECK(header.uriOffset == sizeof(TrackColdHeader));
      CHECK(header.uriLength == 3);
      CHECK(view.property().uri() == "abc");
    }
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
    auto context = TrackSerializationContext{};
    auto builder = TrackBuilder::createNew();
    builder.metadata()
      .trackNumber(5)
      .trackTotal(10)
      .discNumber(1)
      .discTotal(2)
      .conductor("Carlos Kleiber")
      .ensemble("Vienna Philharmonic")
      .soloist("Yo-Yo Ma");
    builder.property().uri("/path/to/file.flac").duration(std::chrono::minutes{3});

    auto const [hotData, coldData] = context.serialize(builder);

    auto const* header = reinterpret_cast<TrackColdHeader const*>(coldData.data());
    CHECK(header->duration == std::chrono::minutes{3});
    CHECK(header->trackNumber == 5);
    CHECK(header->trackTotal == 10);
    CHECK(header->discNumber == 1);
    CHECK(header->discTotal == 2);
    CHECK(coldBlockOffset(*header, TrackColdBlockSlot::Classical) == sizeof(TrackColdHeader));
    CHECK(header->uriOffset == sizeof(TrackColdHeader) + sizeof(TrackClassicalBlock));

    auto const view = TrackView{std::span<std::byte const>{}, coldData};
    CHECK(view.classical().conductorId() == context.dict().getId("Carlos Kleiber"));
    CHECK(view.classical().ensembleId() == context.dict().getId("Vienna Philharmonic"));
    CHECK(view.classical().soloistId() == context.dict().getId("Yo-Yo Ma"));
  }

  TEST_CASE("TrackBuilder - writes extension blocks in deterministic order",
            "[library][unit][track][builder][serialization]")
  {
    auto context = TrackSerializationContext{};
    auto builder = TrackBuilder::createNew();
    builder.coverArt().add(PictureType::FrontCover, ResourceId{42});
    builder.metadata().work("Work");
    builder.customMetadata().add("key", "value");

    auto const coldData = context.serializeCold(builder);
    auto const& header = coldHeader(coldData);
    auto const slots = coldBlockSlots(coldData);

    REQUIRE(slots.size() == 3);
    CHECK(slots[0] == TrackColdBlockSlot::CoverArt);
    CHECK(slots[1] == TrackColdBlockSlot::Classical);
    CHECK(slots[2] == TrackColdBlockSlot::CustomMetadata);
    CHECK(coldBlockOffset(header, TrackColdBlockSlot::CoverArt) == sizeof(TrackColdHeader));
    CHECK(coldBlockOffset(header, TrackColdBlockSlot::CoverArt) <
          coldBlockOffset(header, TrackColdBlockSlot::Classical));
    CHECK(coldBlockOffset(header, TrackColdBlockSlot::Classical) <
          coldBlockOffset(header, TrackColdBlockSlot::CustomMetadata));
    CHECK(coldBlockOffset(header, TrackColdBlockSlot::CustomMetadata) < header.uriOffset);
  }

  TEST_CASE("TrackBuilder - writes custom block logical length and aligned padding",
            "[library][unit][track][builder][serialization]")
  {
    auto context = TrackSerializationContext{};
    auto builder = TrackBuilder::createNew();
    builder.customMetadata().add("odd", "abc");
    builder.property().uri("uri");

    auto const coldData = context.serializeCold(builder);
    auto const& header = coldHeader(coldData);
    auto const customOffset = coldBlockOffset(header, TrackColdBlockSlot::CustomMetadata);
    auto const expectedPayloadLength =
      sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry) + std::string_view{"abc"}.size();
    auto const expectedAlignedPayloadLength = alignToWord(expectedPayloadLength);

    REQUIRE(customOffset == sizeof(TrackColdHeader));
    REQUIRE(header.uriOffset == customOffset + expectedAlignedPayloadLength);

    auto const customPayload =
      std::span<std::byte const>{coldData}.subspan(customOffset, header.uriOffset - customOffset);
    auto const* customHeader = utility::layout::view<CustomMetadataBlockHeader>(customPayload);
    REQUIRE(customHeader != nullptr);
    CHECK(customHeader->entryCount == 1);
    CHECK(customHeader->valueOffset == sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry));
    CHECK(customHeader->payloadLength == expectedPayloadLength);

    auto const* entry = utility::layout::view<CustomMetadataEntry>(
      customPayload.subspan(sizeof(CustomMetadataBlockHeader), sizeof(CustomMetadataEntry)));
    REQUIRE(entry != nullptr);
    CHECK(entry->keyId == context.dict().getId("odd"));
    CHECK(entry->valueOffset == customHeader->valueOffset);
    CHECK(entry->valueLength == 3);

    auto const customPadding = customPayload.subspan(customHeader->payloadLength);
    CHECK(std::ranges::all_of(customPadding, [](std::byte value) { return value == std::byte{0}; }));

    auto const uriPadding = std::span<std::byte const>{coldData}.subspan(header.uriOffset + header.uriLength);
    CHECK(std::ranges::all_of(uriPadding, [](std::byte value) { return value == std::byte{0}; }));

    auto const view = TrackView{std::span<std::byte const>{}, coldData};
    REQUIRE(view.customMetadata().get(context.dict().getId("odd")));
    CHECK(*view.customMetadata().get(context.dict().getId("odd")) == "abc");
    CHECK(view.property().uri() == "uri");
  }

  TEST_CASE("TrackBuilder - writes classical block for standalone new classical fields",
            "[library][unit][track][builder][serialization]")
  {
    auto expectSingleClassicalBlock = [](auto configure, auto check)
    {
      auto context = TrackSerializationContext{};
      auto builder = TrackBuilder::createNew();
      configure(builder);

      auto const coldData = context.serializeCold(builder);
      auto const& header = coldHeader(coldData);
      auto const slots = coldBlockSlots(coldData);
      auto const view = TrackView{std::span<std::byte const>{}, coldData};

      REQUIRE(slots.size() == 1);
      CHECK(slots[0] == TrackColdBlockSlot::Classical);
      CHECK(coldBlockOffset(header, TrackColdBlockSlot::Classical) == sizeof(TrackColdHeader));
      check(view, context.dict());
    };

    SECTION("conductor")
    {
      expectSingleClassicalBlock([](TrackBuilder& builder) { builder.metadata().conductor("Conductor"); },
                                 [](TrackView const& view, DictionaryStore& dict)
                                 { CHECK(view.classical().conductorId() == dict.getId("Conductor")); });
    }

    SECTION("ensemble")
    {
      expectSingleClassicalBlock([](TrackBuilder& builder) { builder.metadata().ensemble("Ensemble"); },
                                 [](TrackView const& view, DictionaryStore& dict)
                                 { CHECK(view.classical().ensembleId() == dict.getId("Ensemble")); });
    }

    SECTION("soloist")
    {
      expectSingleClassicalBlock([](TrackBuilder& builder) { builder.metadata().soloist("Soloist"); },
                                 [](TrackView const& view, DictionaryStore& dict)
                                 { CHECK(view.classical().soloistId() == dict.getId("Soloist")); });
    }
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
    original.metadata()
      .title("Title")
      .albumArtist("Test Album Artist")
      .composer("Test Composer")
      .conductor("Test Conductor")
      .ensemble("Test Ensemble")
      .soloist("Test Soloist");
    original.property().uri("/path.flac");

    auto const [hotData, coldData] = context.serialize(original);
    auto view = TrackView{hotData, coldData};

    auto reconstructed = TrackBuilder::fromView(view, context.dict());
    CHECK(reconstructed.metadata().title() == "Title");
    CHECK(reconstructed.metadata().albumArtist() == "Test Album Artist");
    CHECK(reconstructed.metadata().composer() == "Test Composer");
    CHECK(reconstructed.metadata().conductor() == "Test Conductor");
    CHECK(reconstructed.metadata().ensemble() == "Test Ensemble");
    CHECK(reconstructed.metadata().soloist() == "Test Soloist");
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
      .albumArtist("Album Artist")
      .conductor("Conductor")
      .ensemble("Ensemble")
      .soloist("Soloist");
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
    CHECK(view.classical().conductorId() == context.dict().getId("Conductor"));
    CHECK(view.classical().ensembleId() == context.dict().getId("Ensemble"));
    CHECK(view.classical().soloistId() == context.dict().getId("Soloist"));
  }
} // namespace ao::library::test
