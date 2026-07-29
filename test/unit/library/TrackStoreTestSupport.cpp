// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackStoreTestSupport.h"

#include "WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  TrackStoreFixture::TrackStoreFixture()
    : temp{}, library{temp.path(), temp.path() / "db"}, store{library.tracks()}
  {
  }

  std::vector<std::byte> makeHotData(TrackHotHeader header, std::string_view title)
  {
    header.titleLength = static_cast<std::uint16_t>(title.size());

    auto data = std::vector<std::byte>(alignToWord(sizeof(TrackHotHeader) + title.size()), std::byte{0});
    std::memcpy(data.data(), &header, sizeof(TrackHotHeader));

    if (!title.empty())
    {
      std::memcpy(data.data() + sizeof(TrackHotHeader), title.data(), title.size());
    }

    return data;
  }

  std::vector<std::byte> makeColdData(TrackColdHeader header)
  {
    header.blockOffsets = {};
    header.uriOffset = sizeof(TrackColdHeader);
    header.uriLength = 0;

    auto data = std::vector<std::byte>(sizeof(TrackColdHeader), std::byte{0});
    std::memcpy(data.data(), &header, sizeof(TrackColdHeader));
    return data;
  }

  TrackId createCommittedTrack(TrackStore const& store,
                               MusicLibrary& library,
                               std::span<std::byte const> hotData,
                               std::span<std::byte const> coldData)
  {
    auto wtxn = writeTransaction(library);
    auto created = requireCreate(store.writer(wtxn), hotData, coldData);
    REQUIRE(wtxn.commit());
    return created.first;
  }
} // namespace ao::library::test
