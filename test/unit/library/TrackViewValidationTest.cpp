// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackViewTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    constexpr std::size_t alignToWord(std::size_t size) noexcept
    {
      return ((size + 3U) / 4U) * 4U;
    }

    template<typename T>
    void writePod(std::vector<std::byte>& data, std::size_t offset, T const& value)
    {
      REQUIRE(offset + sizeof(T) <= data.size());
      std::memcpy(data.data() + offset, &value, sizeof(T));
    }

    struct RawColdBlock final
    {
      TrackColdBlockSlot slot = TrackColdBlockSlot::Classical;
      std::vector<std::byte> payload{};
    };

    std::vector<std::byte> makeColdRecord(std::vector<RawColdBlock> const& blocks)
    {
      auto blockOffsets = std::array<std::uint16_t, kTrackColdBlockSlotCount>{};
      std::size_t offset = sizeof(TrackColdHeader);

      for (auto const& block : blocks)
      {
        blockOffsets[trackColdBlockSlotIndex(block.slot)] = static_cast<std::uint16_t>(offset);
        offset = alignToWord(offset + block.payload.size());
      }

      auto const uriOffset = offset;
      auto data = std::vector<std::byte>(uriOffset, std::byte{0});
      writePod(data,
               0,
               TrackColdHeader{
                 .blockOffsets = blockOffsets,
                 .uriOffset = static_cast<std::uint16_t>(uriOffset),
               });

      offset = sizeof(TrackColdHeader);

      for (auto const& block : blocks)
      {
        if (!block.payload.empty())
        {
          REQUIRE(offset + block.payload.size() <= data.size());
          std::memcpy(data.data() + offset, block.payload.data(), block.payload.size());
        }

        offset = alignToWord(offset + block.payload.size());
      }

      return data;
    }

    std::vector<std::byte> makeClassicalPayload()
    {
      auto payload = std::vector<std::byte>(sizeof(TrackClassicalBlock), std::byte{0});
      writePod(payload,
               0,
               TrackClassicalBlock{
                 .workId = DictionaryId{1},
                 .movementId = DictionaryId{2},
                 .movementNumber = 3,
                 .movementTotal = 4,
               });
      return payload;
    }

    std::vector<std::byte> makeCoverPayload()
    {
      auto payload = std::vector<std::byte>(sizeof(CoverArtEntry), std::byte{0});
      writePod(
        payload, 0, CoverArtEntry{.id = ResourceId{42}, .type = static_cast<std::uint8_t>(PictureType::FrontCover)});
      return payload;
    }

    std::vector<std::byte> makeUnsortedCustomPayload()
    {
      auto payload =
        std::vector<std::byte>(sizeof(CustomMetadataBlockHeader) + (2 * sizeof(CustomMetadataEntry)), std::byte{0});
      auto const valueOffset = static_cast<std::uint16_t>(payload.size());
      writePod(
        payload,
        0,
        CustomMetadataBlockHeader{
          .entryCount = 2, .valueOffset = valueOffset, .payloadLength = static_cast<std::uint16_t>(payload.size())});
      writePod(payload,
               sizeof(CustomMetadataBlockHeader),
               CustomMetadataEntry{.keyId = DictionaryId{2}, .valueOffset = valueOffset});
      writePod(payload,
               sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry),
               CustomMetadataEntry{.keyId = DictionaryId{1}, .valueOffset = valueOffset});
      return payload;
    }

    /** The deep verifier must reject the record. */
    void expectVerifierRejects(std::vector<std::byte> const& data)
    {
      auto const reader = detail::TrackColdReader{data};
      CHECK_FALSE(reader.valid());
    }

    /** The read gate must poison the whole cold side, record-granular. */
    void expectColdGateRejects(std::vector<std::byte> const& data)
    {
      expectVerifierRejects(data);

      auto const view = TrackView{std::span<std::byte const>{}, data};
      CHECK_FALSE(view.isColdValid());
      CHECK(view.metadata().trackNumber() == 0);
      CHECK(view.property().uri().empty());
      CHECK(view.classical().empty());
      CHECK(view.coverArt().count() == 0);
      CHECK(view.customMetadata().count() == 0);
    }

    /** The deep verifier rejects, but the read gate tolerates the record. */
    TrackView expectColdGateTolerates(std::vector<std::byte> const& data)
    {
      expectVerifierRejects(data);

      auto view = TrackView{std::span<std::byte const>{}, data};
      CHECK(view.isColdValid());
      return view;
    }
  } // namespace

  TEST_CASE("TrackView - validates hot buffers", "[library][unit][track][validation]")
  {
    auto const data = makeMinimalHotTrackViewData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.isHotValid() == true);
  }

  TEST_CASE("TrackView - rejects null hot data", "[library][unit][track][validation]")
  {
    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    auto const nullView = TrackView{nullSpan, std::span<std::byte const>{}};
    CHECK(nullView.isHotValid() == false);
  }

  TEST_CASE("TrackView - rejects undersized hot data", "[library][unit][track][validation]")
  {
    auto const smallData = std::array<char, 10>{};
    auto const smallView = TrackView{utility::bytes::view(smallData), std::span<std::byte const>{}};
    CHECK(smallView.isHotValid() == false);
  }

  TEST_CASE("TrackView hot gate - poisons records whose extents overrun the record",
            "[library][unit][track][validation]")
  {
    SECTION("title length overruns the record")
    {
      auto data = makeMinimalHotTrackViewData();
      auto header = makeMinimalTrackHotHeader();
      header.titleLength = 100;
      writePod(data, 0, header);

      auto const view = TrackView{data, std::span<std::byte const>{}};
      CHECK_FALSE(view.isHotValid());
      CHECK(view.metadata().title().empty());
      CHECK(view.metadata().artistId() == kInvalidDictionaryId);
      CHECK(view.tags().count() == 0);
      CHECK(view.tags().bloom() == 0);
    }

    SECTION("tag length overruns the record")
    {
      auto data = makeMinimalHotTrackViewData();
      auto header = makeMinimalTrackHotHeader();
      header.tagLength = 64;
      writePod(data, 0, header);

      auto const view = TrackView{data, std::span<std::byte const>{}};
      CHECK_FALSE(view.isHotValid());
      CHECK(view.tags().count() == 0);
    }

    SECTION("tag region is not a whole number of tag IDs")
    {
      auto data = makeMinimalHotTrackViewData();
      data.resize(data.size() + 4, std::byte{0});
      auto header = makeMinimalTrackHotHeader();
      header.tagLength = 2;
      writePod(data, 0, header);

      auto const view = TrackView{data, std::span<std::byte const>{}};
      CHECK_FALSE(view.isHotValid());
      CHECK(view.tags().count() == 0);
    }
  }

  TEST_CASE("TrackView - validates cold buffers", "[library][unit][track][validation]")
  {
    auto const data = makeColdTrackViewData();
    auto const view = TrackView{std::span<std::byte const>{}, data};
    CHECK(view.isColdValid() == true);
  }

  TEST_CASE("TrackView - rejects null cold data", "[library][unit][track][validation]")
  {
    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    auto const nullView = TrackView{std::span<std::byte const>{}, nullSpan};
    CHECK(nullView.isColdValid() == false);
  }

  TEST_CASE("TrackView - rejects undersized cold data", "[library][unit][track][validation]")
  {
    auto const smallData = std::array<char, 10>{};
    auto const smallView = TrackView{std::span<std::byte const>{}, utility::bytes::view(smallData)};
    CHECK(smallView.isColdValid() == false);
  }

  TEST_CASE("TrackColdReader - validates extension block records", "[library][unit][track][validation]")
  {
    auto const data = makeColdRecord({RawColdBlock{.payload = makeClassicalPayload()}});
    auto const reader = detail::TrackColdReader{data};
    REQUIRE(reader.valid());
    CHECK(reader.classical().workId() == DictionaryId{1});
    CHECK(reader.classical().movementNumber() == 3);
  }

  TEST_CASE("TrackView - block-backed proxies are stable across repeated access and copies",
            "[library][unit][track][validation]")
  {
    auto const data = makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CoverArt, .payload = makeCoverPayload()},
                                      RawColdBlock{.payload = makeClassicalPayload()}});
    auto const view = TrackView{std::span<std::byte const>{}, data};

    CHECK(view.classical().workId() == DictionaryId{1});
    CHECK(view.classical().workId() == DictionaryId{1});
    REQUIRE(view.coverArt().primary());
    CHECK(view.coverArt().primary()->resourceId == ResourceId{42});

    auto const copy = view;
    CHECK(copy.classical().movementNumber() == 3);
    REQUIRE(copy.coverArt().primary());
    CHECK(copy.coverArt().primary()->type == PictureType::FrontCover);
  }

  TEST_CASE("TrackView cold gate - poisons structurally unsafe records", "[library][unit][track][validation]")
  {
    SECTION("too short for header")
    {
      auto const data = std::vector<std::byte>(sizeof(TrackColdHeader) - 1, std::byte{0});
      expectColdGateRejects(data);
    }

    SECTION("URI range overruns the record")
    {
      auto data = std::vector<std::byte>(sizeof(TrackColdHeader), std::byte{0});
      writePod(data,
               0,
               TrackColdHeader{
                 .uriOffset = static_cast<std::uint16_t>(sizeof(TrackColdHeader)),
                 .uriLength = 8,
               });
      expectColdGateRejects(data);
    }

    SECTION("URI offset points inside the header")
    {
      auto data = std::vector<std::byte>(sizeof(TrackColdHeader), std::byte{0});
      writePod(data, 0, TrackColdHeader{.uriOffset = 8, .uriLength = 0});
      expectColdGateRejects(data);
    }

    SECTION("slot offset is not aligned")
    {
      auto data = makeColdRecord({RawColdBlock{.payload = makeClassicalPayload()}});
      auto* header = utility::layout::viewMutable<TrackColdHeader>(data);
      header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Classical)] = 30;
      expectColdGateRejects(data);
    }

    SECTION("slot offset is before the header")
    {
      auto data = makeColdRecord({RawColdBlock{.payload = makeClassicalPayload()}});
      auto* header = utility::layout::viewMutable<TrackColdHeader>(data);
      header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Classical)] =
        static_cast<std::uint16_t>(sizeof(TrackColdHeader) - 4);
      expectColdGateRejects(data);
    }

    SECTION("present slot offsets are not strictly increasing")
    {
      auto data = makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CoverArt, .payload = makeCoverPayload()},
                                  RawColdBlock{.payload = makeClassicalPayload()}});
      auto* header = utility::layout::viewMutable<TrackColdHeader>(data);
      header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Classical)] =
        header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::CoverArt)];
      expectColdGateRejects(data);
    }

    SECTION("slot offset points at the URI")
    {
      auto data = makeColdRecord({RawColdBlock{.payload = makeClassicalPayload()}});
      auto* header = utility::layout::viewMutable<TrackColdHeader>(data);
      header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Classical)] = header->uriOffset;
      expectColdGateRejects(data);
    }

    SECTION("classical slice is too small for the block")
    {
      auto const data = makeColdRecord({RawColdBlock{.payload = std::vector<std::byte>(4, std::byte{0})}});
      expectColdGateRejects(data);
    }

    SECTION("classical slice shrinks below the block size through a shifted offset")
    {
      auto data = makeColdRecord({RawColdBlock{.payload = makeClassicalPayload()}});
      auto* header = utility::layout::viewMutable<TrackColdHeader>(data);
      header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Classical)] =
        static_cast<std::uint16_t>(sizeof(TrackColdHeader) + 4);
      expectColdGateRejects(data);
    }

    SECTION("custom payload is too short for its block header")
    {
      auto const data = makeColdRecord(
        {RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::vector<std::byte>(1, std::byte{0})}});
      expectColdGateRejects(data);
    }

    SECTION("custom entry table overruns the payload")
    {
      auto payload = std::vector<std::byte>(sizeof(CustomMetadataBlockHeader), std::byte{0});
      writePod(payload,
               0,
               CustomMetadataBlockHeader{.entryCount = 1,
                                         .valueOffset = static_cast<std::uint16_t>(sizeof(CustomMetadataBlockHeader) +
                                                                                   sizeof(CustomMetadataEntry)),
                                         .payloadLength = static_cast<std::uint16_t>(payload.size())});

      auto const data =
        makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::move(payload)}});
      expectColdGateRejects(data);
    }

    SECTION("a later invalid block poisons earlier valid blocks")
    {
      auto const data = makeColdRecord(
        {RawColdBlock{.payload = makeClassicalPayload()},
         RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::vector<std::byte>(1, std::byte{0})}});
      expectColdGateRejects(data);
    }

    SECTION("malformed cold structure poisons fixed cold fields too")
    {
      auto data = makeColdRecord({RawColdBlock{.payload = makeClassicalPayload()}});
      auto* header = utility::layout::viewMutable<TrackColdHeader>(data);
      header->trackNumber = 7;
      header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::Classical)] = 30;

      auto const view = TrackView{std::span<std::byte const>{}, data};
      CHECK_FALSE(view.isColdValid());
      CHECK(view.metadata().trackNumber() == 0);
      CHECK(view.property().uri().empty());
      CHECK(view.classical().empty());
    }
  }

  TEST_CASE("TrackView cold gate - tolerates semantic corruption within bounds", "[library][unit][track][validation]")
  {
    SECTION("header reserved byte is nonzero")
    {
      auto data = makeColdRecord({RawColdBlock{.payload = makeClassicalPayload()}});
      auto* header = utility::layout::viewMutable<TrackColdHeader>(data);
      header->reserved8 = 1;

      auto const view = expectColdGateTolerates(data);
      CHECK(view.classical().workId() == DictionaryId{1});
    }

    SECTION("reserved slot is nonzero")
    {
      auto data = makeColdRecord({RawColdBlock{.payload = makeClassicalPayload()}});
      auto* header = utility::layout::viewMutable<TrackColdHeader>(data);
      header->blockOffsets[kTrackColdKnownBlockSlotCount] = header->uriOffset;

      auto const view = expectColdGateTolerates(data);
      CHECK(view.classical().workId() == DictionaryId{1});
    }

    SECTION("URI padding is nonzero")
    {
      auto data = std::vector<std::byte>(sizeof(TrackColdHeader) + 4, std::byte{0});
      writePod(data,
               0,
               TrackColdHeader{
                 .uriOffset = static_cast<std::uint16_t>(sizeof(TrackColdHeader)),
                 .uriLength = 1,
               });
      data[sizeof(TrackColdHeader)] = std::byte{0x75};
      data[sizeof(TrackColdHeader) + 1] = std::byte{1};

      auto const view = expectColdGateTolerates(data);
      CHECK(view.property().uri() == "u");
    }

    SECTION("record has trailing bytes after URI padding")
    {
      auto data = makeColdRecord({RawColdBlock{.payload = makeClassicalPayload()}});
      data.resize(data.size() + 4, std::byte{0});

      auto const view = expectColdGateTolerates(data);
      CHECK(view.classical().workId() == DictionaryId{1});
    }

    SECTION("record size exceeds the writer's uint16 maximum")
    {
      constexpr auto kOversizedRecordSize = static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1U;
      auto data = std::vector<std::byte>(kOversizedRecordSize, std::byte{0});
      writePod(data,
               0,
               TrackColdHeader{
                 .uriOffset = static_cast<std::uint16_t>(sizeof(TrackColdHeader)),
                 .uriLength = static_cast<std::uint16_t>(kOversizedRecordSize - sizeof(TrackColdHeader)),
               });
      expectColdGateTolerates(data);
    }

    SECTION("cover slice that is not a whole number of entries reads as empty cover art")
    {
      auto const data = makeColdRecord(
        {RawColdBlock{.slot = TrackColdBlockSlot::CoverArt, .payload = std::vector<std::byte>(1, std::byte{0})}});

      auto const view = expectColdGateTolerates(data);
      CHECK(view.coverArt().count() == 0);
      CHECK_FALSE(view.coverArt().primary());
    }

    SECTION("cover entry semantic garbage is passed through")
    {
      auto data = makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CoverArt, .payload = makeCoverPayload()}});
      auto const* header = utility::layout::view<TrackColdHeader>(data);
      REQUIRE(header != nullptr);
      auto* entry = utility::layout::viewMutable<CoverArtEntry>(
        std::span{data}.subspan(header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::CoverArt)]));
      REQUIRE(entry != nullptr);
      entry->type = static_cast<std::uint8_t>(PictureType::PublisherLogo) + 1;
      entry->reserved[0] = 1;

      auto const view = expectColdGateTolerates(data);
      CHECK(view.coverArt().count() == 1);
      CHECK(view.coverArt().at(0).resourceId == ResourceId{42});
    }

    SECTION("small unsorted custom tables stay readable through the linear-search path")
    {
      auto const data = makeColdRecord(
        {RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = makeUnsortedCustomPayload()}});

      auto const view = expectColdGateTolerates(data);
      CHECK(view.customMetadata().count() == 2);
      CHECK(view.customMetadata().contains(DictionaryId{1}));
      CHECK(view.customMetadata().contains(DictionaryId{2}));
    }

    SECTION("large unsorted custom tables stay memory-safe without lookup guarantees")
    {
      // 64 entries reach the binary-search lookup path, which assumes the
      // writer-sorted key order. On unsorted data iteration stays complete
      // and in stored order; keyed lookup may miss and is deliberately not
      // asserted - only that it runs within bounds.
      constexpr std::uint16_t kEntryCount = 64;

      auto payload = std::vector<std::byte>(
        sizeof(CustomMetadataBlockHeader) + (kEntryCount * sizeof(CustomMetadataEntry)), std::byte{0});
      auto const valueOffset = static_cast<std::uint16_t>(payload.size());
      writePod(payload,
               0,
               CustomMetadataBlockHeader{.entryCount = kEntryCount,
                                         .valueOffset = valueOffset,
                                         .payloadLength = static_cast<std::uint16_t>(payload.size())});

      for (std::uint16_t i = 0; i < kEntryCount; ++i)
      {
        writePod(payload,
                 sizeof(CustomMetadataBlockHeader) + (i * sizeof(CustomMetadataEntry)),
                 CustomMetadataEntry{
                   .keyId = DictionaryId{static_cast<std::uint32_t>(kEntryCount - i)}, .valueOffset = valueOffset});
      }

      auto const data =
        makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::move(payload)}});

      auto const view = expectColdGateTolerates(data);
      CHECK(view.customMetadata().count() == kEntryCount);

      std::size_t iterated = 0;
      bool sawFirstStoredKey = false;

      for (auto const& [keyId, value] : view.customMetadata())
      {
        ++iterated;
        sawFirstStoredKey = sawFirstStoredKey || keyId == DictionaryId{kEntryCount};
        CHECK(value.empty());
      }

      CHECK(iterated == kEntryCount);
      CHECK(sawFirstStoredKey);

      std::ignore = view.customMetadata().contains(DictionaryId{kEntryCount});
      std::ignore = view.customMetadata().get(DictionaryId{1});
    }

    SECTION("custom entry value range outside the payload clamps to an empty value")
    {
      auto payload =
        std::vector<std::byte>(sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry), std::byte{0});
      auto const valueOffset = static_cast<std::uint16_t>(payload.size());
      writePod(
        payload,
        0,
        CustomMetadataBlockHeader{
          .entryCount = 1, .valueOffset = valueOffset, .payloadLength = static_cast<std::uint16_t>(payload.size())});
      writePod(payload,
               sizeof(CustomMetadataBlockHeader),
               CustomMetadataEntry{.keyId = DictionaryId{1}, .valueOffset = 255, .valueLength = 1});

      auto const data =
        makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::move(payload)}});

      auto const view = expectColdGateTolerates(data);
      auto const optValue = view.customMetadata().get(DictionaryId{1});
      REQUIRE(optValue);
      CHECK(optValue->empty());
    }
  }

  TEST_CASE("TrackColdReader - rejects malformed extension block records", "[library][unit][track][validation]")
  {
    SECTION("first present slot leaves a post-header gap")
    {
      auto data = makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CoverArt, .payload = makeCoverPayload()},
                                  RawColdBlock{.payload = makeClassicalPayload()}});
      auto* header = utility::layout::viewMutable<TrackColdHeader>(data);
      header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::CoverArt)] = 0;
      expectVerifierRejects(data);
    }

    SECTION("cover payload is not an entry array")
    {
      auto const data = makeColdRecord(
        {RawColdBlock{.slot = TrackColdBlockSlot::CoverArt, .payload = std::vector<std::byte>(1, std::byte{0})}});
      expectVerifierRejects(data);
    }

    SECTION("cover entry has invalid picture type")
    {
      auto data = makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CoverArt, .payload = makeCoverPayload()}});
      auto const* header = utility::layout::view<TrackColdHeader>(data);
      REQUIRE(header != nullptr);
      auto* entry = utility::layout::viewMutable<CoverArtEntry>(
        std::span{data}.subspan(header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::CoverArt)]));
      REQUIRE(entry != nullptr);
      entry->type = static_cast<std::uint8_t>(PictureType::PublisherLogo) + 1;
      expectVerifierRejects(data);
    }

    SECTION("cover entry has invalid resource id")
    {
      auto data = makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CoverArt, .payload = makeCoverPayload()}});
      auto const* header = utility::layout::view<TrackColdHeader>(data);
      REQUIRE(header != nullptr);
      auto* entry = utility::layout::viewMutable<CoverArtEntry>(
        std::span{data}.subspan(header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::CoverArt)]));
      REQUIRE(entry != nullptr);
      entry->id = kInvalidResourceId;
      expectVerifierRejects(data);
    }

    SECTION("cover entry reserved bytes are nonzero")
    {
      auto data = makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CoverArt, .payload = makeCoverPayload()}});
      auto const* header = utility::layout::view<TrackColdHeader>(data);
      REQUIRE(header != nullptr);
      auto* entry = utility::layout::viewMutable<CoverArtEntry>(
        std::span{data}.subspan(header->blockOffsets[trackColdBlockSlotIndex(TrackColdBlockSlot::CoverArt)]));
      REQUIRE(entry != nullptr);
      entry->reserved[0] = 1;
      expectVerifierRejects(data);
    }

    SECTION("custom header value offset disagrees with computed table end")
    {
      auto payload =
        std::vector<std::byte>(sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry) + 4, std::byte{0});
      auto const wrongValueOffset = static_cast<std::uint16_t>(payload.size());
      writePod(payload,
               0,
               CustomMetadataBlockHeader{.entryCount = 1,
                                         .valueOffset = wrongValueOffset,
                                         .payloadLength = static_cast<std::uint16_t>(payload.size())});
      writePod(payload,
               sizeof(CustomMetadataBlockHeader),
               CustomMetadataEntry{.keyId = DictionaryId{1}, .valueOffset = wrongValueOffset});

      auto const data =
        makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::move(payload)}});
      expectVerifierRejects(data);
    }

    SECTION("custom entry value starts before value area")
    {
      auto payload =
        std::vector<std::byte>(sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry) + 4, std::byte{0});
      auto const valueOffset =
        static_cast<std::uint16_t>(sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry));
      writePod(
        payload,
        0,
        CustomMetadataBlockHeader{
          .entryCount = 1, .valueOffset = valueOffset, .payloadLength = static_cast<std::uint16_t>(payload.size())});
      writePod(
        payload,
        sizeof(CustomMetadataBlockHeader),
        CustomMetadataEntry{
          .keyId = DictionaryId{1}, .valueOffset = static_cast<std::uint16_t>(valueOffset - 1), .valueLength = 1});

      auto const data =
        makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::move(payload)}});
      expectVerifierRejects(data);
    }

    SECTION("custom entry value offset is outside payload")
    {
      auto payload =
        std::vector<std::byte>(sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry), std::byte{0});
      auto const valueOffset = static_cast<std::uint16_t>(payload.size());
      writePod(
        payload,
        0,
        CustomMetadataBlockHeader{
          .entryCount = 1, .valueOffset = valueOffset, .payloadLength = static_cast<std::uint16_t>(payload.size())});
      writePod(payload,
               sizeof(CustomMetadataBlockHeader),
               CustomMetadataEntry{.keyId = DictionaryId{1}, .valueOffset = 255, .valueLength = 1});

      auto const data =
        makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::move(payload)}});
      expectVerifierRejects(data);
    }

    SECTION("custom entry has invalid key id")
    {
      auto payload =
        std::vector<std::byte>(sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry), std::byte{0});
      auto const valueOffset = static_cast<std::uint16_t>(payload.size());
      writePod(
        payload,
        0,
        CustomMetadataBlockHeader{
          .entryCount = 1, .valueOffset = valueOffset, .payloadLength = static_cast<std::uint16_t>(payload.size())});
      writePod(payload,
               sizeof(CustomMetadataBlockHeader),
               CustomMetadataEntry{.keyId = kInvalidDictionaryId, .valueOffset = valueOffset});

      auto const data =
        makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::move(payload)}});
      expectVerifierRejects(data);
    }

    SECTION("custom entries are not sorted")
    {
      auto const data = makeColdRecord(
        {RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = makeUnsortedCustomPayload()}});
      expectVerifierRejects(data);
    }

    SECTION("custom logical payload length disagrees with aligned slot span")
    {
      auto payload = std::vector<std::byte>(sizeof(CustomMetadataBlockHeader), std::byte{0});
      writePod(payload,
               0,
               CustomMetadataBlockHeader{.entryCount = 0,
                                         .valueOffset = static_cast<std::uint16_t>(sizeof(CustomMetadataBlockHeader)),
                                         .payloadLength = static_cast<std::uint16_t>(payload.size() - 1)});

      auto const data =
        makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::move(payload)}});
      expectVerifierRejects(data);
    }

    SECTION("custom aligned padding is nonzero")
    {
      auto payload =
        std::vector<std::byte>(sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry) + 1, std::byte{0});
      auto const valueOffset =
        static_cast<std::uint16_t>(sizeof(CustomMetadataBlockHeader) + sizeof(CustomMetadataEntry));
      writePod(
        payload,
        0,
        CustomMetadataBlockHeader{
          .entryCount = 1, .valueOffset = valueOffset, .payloadLength = static_cast<std::uint16_t>(payload.size())});
      writePod(payload,
               sizeof(CustomMetadataBlockHeader),
               CustomMetadataEntry{.keyId = DictionaryId{1}, .valueOffset = valueOffset, .valueLength = 1});

      auto data =
        makeColdRecord({RawColdBlock{.slot = TrackColdBlockSlot::CustomMetadata, .payload = std::move(payload)}});
      data[sizeof(TrackColdHeader) + valueOffset + 1] = std::byte{1};
      expectVerifierRejects(data);
    }
  }
} // namespace ao::library::test
