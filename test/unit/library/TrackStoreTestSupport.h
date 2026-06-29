// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  constexpr std::size_t alignToWord(std::size_t size) noexcept
  {
    return ((size + 3U) / 4U) * 4U;
  }

  inline TrackStore openTrackStore(Environment& env)
  {
    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());
    return store;
  }

  struct TrackStoreFixture final
  {
    ao::test::TempDir temp;
    Environment env;
    TrackStore store;

    TrackStoreFixture()
      : temp{}, env{openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20})}, store{openTrackStore(env)}
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

  inline TrackId createCommittedTrack(TrackStore& store,
                                      Environment& env,
                                      std::span<std::byte const> hotData,
                                      std::span<std::byte const> coldData)
  {
    auto wtxn = beginWriteTransaction(env);
    auto created = requireCreate(store.writer(wtxn), hotData, coldData);
    REQUIRE(wtxn.commit());
    return created.first;
  }
} // namespace ao::library::test
