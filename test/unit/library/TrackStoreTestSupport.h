// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library::test
{
  constexpr std::size_t alignToWord(std::size_t size) noexcept
  {
    return ((size + 3U) / 4U) * 4U;
  }

  struct TrackStoreFixture final
  {
    ao::test::TempDir temp;
    MusicLibrary library;
    TrackStore const& store;

    TrackStoreFixture()
      : temp{}, library{temp.path(), temp.path() / "db"}, store{library.tracks()}
    {
    }
  };

  inline std::vector<std::byte> makeHotData(TrackHotHeader header = {}, std::string_view title = {})
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

  inline std::vector<std::byte> makeColdData(TrackColdHeader header = {})
  {
    header.blockOffsets = {};
    header.uriOffset = sizeof(TrackColdHeader);
    header.uriLength = 0;

    auto data = std::vector<std::byte>(sizeof(TrackColdHeader), std::byte{0});
    std::memcpy(data.data(), &header, sizeof(TrackColdHeader));
    return data;
  }

  template<typename Writer>
  std::pair<TrackId, TrackView> requireCreate(Writer&& writer,
                                              std::span<std::byte const> hotData,
                                              std::span<std::byte const> coldData)
  {
    auto result = std::forward<Writer>(writer).createHotCold(hotData, coldData);
    REQUIRE(result);
    return *result;
  }

  inline TrackId createCommittedTrack(TrackStore const& store,
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
