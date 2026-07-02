// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TestUtils.h"
#include "test/unit/library/TrackViewTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  namespace
  {
#if defined(__GNUC__) && !defined(__clang__)
    static_assert(std::ranges::view<TrackView::TagProxy>);
#endif
  } // namespace

  TEST_CASE("TrackView - exposes tag bloom filters", "[library][unit][track][tag]")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0xCAFE;

    auto const data = serializeHeader(h);
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().bloom() == 0xCAFE);
  }

  TEST_CASE("TrackView - returns zero tag count without tags", "[library][unit][track][tag]")
  {
    auto const data = makeHotTrackViewData("Test");
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().count() == 0);
  }

  TEST_CASE("TrackView - iterates no tags when tag data is empty", "[library][unit][track][tag]")
  {
    auto const data = makeHotTrackViewData("Test");
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().begin() == view.tags().end());
  }

  TEST_CASE("TrackView - returns tag count with tags", "[library][unit][track][tag]")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = kInvalidDictionaryId;
    h.composerId = kInvalidDictionaryId;
    h.year = 2020;
    h.codec = AudioCodec::Unknown;
    h.bitDepth = BitDepth{16};
    h.tagLength = 8; // 2 tags * 4 bytes

    auto const title = std::string{"Test Title"};
    h.titleLength = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    std::uint32_t const tag1 = 10;
    std::uint32_t const tag2 = 20;
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    appendString(data, title);

    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().count() == 2);
  }

  TEST_CASE("TrackView - iterates tag IDs", "[library][unit][track][tag]")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = kInvalidDictionaryId;
    h.composerId = kInvalidDictionaryId;
    h.year = 2020;
    h.codec = AudioCodec::Unknown;
    h.bitDepth = BitDepth{16};
    h.tagLength = 8; // 2 tags * 4 bytes

    auto const title = std::string{"Test Title"};
    h.titleLength = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    std::uint32_t const tag1 = 10;
    std::uint32_t const tag2 = 20;
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    appendString(data, title);

    auto const view = TrackView{data, std::span<std::byte const>{}};

    auto ids = std::vector(view.tags().begin(), view.tags().end());
    CHECK(ids.size() == 2);
    CHECK(ids[0] == DictionaryId{10});
    CHECK(ids[1] == DictionaryId{20});
  }

  TEST_CASE("TrackView - reports whether tag IDs exist", "[library][unit][track][tag]")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = kInvalidDictionaryId;
    h.composerId = kInvalidDictionaryId;
    h.year = 2020;
    h.codec = AudioCodec::Unknown;
    h.bitDepth = BitDepth{16};
    h.tagLength = 8;

    auto data = serializeHeader(h);
    std::uint32_t const tag1 = 10;
    std::uint32_t const tag2 = 20;
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().has(DictionaryId{10}) == true);
    CHECK(view.tags().has(DictionaryId{20}) == true);
    CHECK(view.tags().has(DictionaryId{30}) == false);
  }

  TEST_CASE("TrackView - returns tag IDs by index", "[library][unit][track][tag]")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = kInvalidDictionaryId;
    h.composerId = kInvalidDictionaryId;
    h.year = 2020;
    h.codec = AudioCodec::Unknown;
    h.bitDepth = BitDepth{16};
    h.tagLength = 8;

    auto data = serializeHeader(h);
    std::uint32_t const tag1 = 10;
    std::uint32_t const tag2 = 20;
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().id(0) == DictionaryId{10});
    CHECK(view.tags().id(1) == DictionaryId{20});
  }
} // namespace ao::library::test
