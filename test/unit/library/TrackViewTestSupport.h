// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include "test/unit/library/LibraryBinaryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library::test
{
  inline TrackHotHeader makeMinimalTrackHotHeader()
  {
    auto header = TrackHotHeader{};
    header.tagBloom = 0;
    header.artistId = DictionaryId{1};
    header.albumId = DictionaryId{2};
    header.genreId = DictionaryId{3};
    header.albumArtistId = kInvalidDictionaryId;
    header.composerId = kInvalidDictionaryId;
    header.year = 2020;
    header.codec = AudioCodec::Unknown;
    header.bitDepth = BitDepth{16};
    header.tagLength = 0;
    header.titleLength = 0;
    return header;
  }

  inline std::vector<std::byte> makeMinimalHotTrackViewData()
  {
    return serializeHeader(makeMinimalTrackHotHeader());
  }

  inline std::vector<std::byte> makeHotTrackViewData(std::string_view title)
  {
    auto header = makeMinimalTrackHotHeader();
    header.titleLength = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(header);
    appendString(data, title);
    return data;
  }

  inline std::vector<std::byte> makeColdTrackViewData(
    TrackColdHeader const& header = {},
    std::vector<std::pair<std::string, std::string>> const& customPairs = {},
    std::string_view uri = "")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.property().uri(uri);
    builder.metadata().trackNumber(header.trackNumber);
    builder.metadata().trackTotal(header.trackTotal);
    builder.metadata().discNumber(header.discNumber);
    builder.metadata().discTotal(header.discTotal);
    builder.property().duration(header.duration);
    builder.property().bitrate(header.bitrate);
    builder.property().channels(header.channels);

    for (auto const& [key, value] : customPairs)
    {
      builder.customMetadata().add(key, value);
    }

    auto temp = ao::test::TempDir{};
    auto library = MusicLibrary{temp.path(), temp.path() / "db"};
    auto transaction = writeTransaction(library);
    auto result = builder.serializeCold(transaction, library.resources());
    REQUIRE(result);
    return *result;
  }

  inline TrackView makeColdTrackView(std::vector<std::byte> const& data)
  {
    return TrackView{std::span<std::byte const>{}, data};
  }
} // namespace ao::library::test
