// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    using namespace ao::lmdb::test;

    std::vector<std::byte> createMinimalHotData()
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
      h.tagLength = 0;
      h.titleLength = 0;

      return serializeHeader(h);
    }

    std::vector<std::byte> createColdData()
    {
      auto builder = TrackBuilder::createNew();

      auto temp = ao::test::TempDir{};
      auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
      auto wtxn = lmdb::test::beginWriteTransaction(env);
      auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
      auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
      auto result = builder.serializeCold(wtxn, dict, resources);
      REQUIRE(result);
      return *result;
    }
  } // namespace

  TEST_CASE("TrackView - validates hot buffers", "[library][unit][track][validation]")
  {
    auto const data = createMinimalHotData();
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

  TEST_CASE("TrackView - validates cold buffers", "[library][unit][track][validation]")
  {
    auto const data = createColdData();
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
} // namespace ao::library::test
