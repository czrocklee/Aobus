// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
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

    TrackStoreFixture();
  };

  std::vector<std::byte> makeHotData(TrackHotHeader header = {}, std::string_view title = {});
  std::vector<std::byte> makeColdData(TrackColdHeader header = {});

  template<typename Writer>
  std::pair<TrackId, TrackView> requireCreate(Writer&& writer,
                                              std::span<std::byte const> hotData,
                                              std::span<std::byte const> coldData)
  {
    auto result = std::forward<Writer>(writer).createHotCold(hotData, coldData);
    REQUIRE(result);
    return *result;
  }

  TrackId createCommittedTrack(TrackStore const& store,
                               MusicLibrary& library,
                               std::span<std::byte const> hotData,
                               std::span<std::byte const> coldData);
} // namespace ao::library::test
