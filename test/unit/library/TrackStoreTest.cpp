// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  namespace
  {
    template<typename Writer>
    std::pair<TrackId, TrackView> requireCreate(Writer&& writer,
                                                std::span<std::byte const> hotData,
                                                std::span<std::byte const> coldData)
    {
      auto result = std::forward<Writer>(writer).createHotCold(hotData, coldData);
      REQUIRE(result);
      return *result;
    }
  } // namespace

  TEST_CASE("TrackStore - create and read", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create a track with hot+cold
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    // Empty cold data (padded to 4-byte alignment)
    auto coldData = std::vector(4, std::byte{0});

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    // If createHotCold() failed, it would throw
    REQUIRE(wtxn2.commit());

    // Read the track
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    CHECK((*it).first == id);
  }

  TEST_CASE("TrackStore - read by id", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create a track with hot+cold
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldData = std::vector(4, std::byte{0});

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Read by ID
    auto rtxn = beginReadTransaction(env);
    auto optFound = store.reader(rtxn).get(id);
    CHECK(optFound.has_value());
  }

  TEST_CASE("TrackStore - update", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create a track
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldData = std::vector(4, std::byte{0});

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Update the track
    auto hotHeader2 = TrackHotHeader{};

    auto hotData2 = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData2.data(), &hotHeader2, sizeof(TrackHotHeader));

    auto wtxn3 = beginWriteTransaction(env);
    REQUIRE(store.writer(wtxn3).updateHot(id, hotData2));
    REQUIRE(wtxn3.commit());
  }

  TEST_CASE("TrackStore - delete", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create a track
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldData = std::vector(4, std::byte{0});

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Delete it
    auto wtxn3 = beginWriteTransaction(env);
    REQUIRE(store.writer(wtxn3).remove(id));
    REQUIRE(wtxn3.commit());

    // Verify it's gone
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    CHECK(it == reader.end());
  }

  TEST_CASE("TrackStore - create multiple tracks unique IDs", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create multiple tracks - each should get unique ID
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldData = std::vector(4, std::byte{0});

    auto wtxn2 = beginWriteTransaction(env);
    auto [id1, view1] = requireCreate(store.writer(wtxn2), hotData, coldData);
    auto [id2, view2] = requireCreate(store.writer(wtxn2), hotData, coldData);
    auto [id3, view3] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // All IDs should be unique
    CHECK(id1 != id2);
    CHECK(id2 != id3);
    CHECK(id1 != id3);
  }

  TEST_CASE("TrackStore - hot/cold createHotCold", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create hot header
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    // Create cold header
    auto coldHeader = TrackColdHeader{};
    coldHeader.duration = std::chrono::minutes{3};
    coldHeader.trackNumber = 1;
    coldHeader.trackTotal = 10;

    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    // Create with hot+cold
    auto wtxn2 = beginWriteTransaction(env);
    auto [id, hotView] = requireCreate(store.writer(wtxn2), hotData, coldData);
    // REQUIRE(id.raw() >= 0);
    REQUIRE(wtxn2.commit());

    // Verify hot and cold data
    auto rtxn = beginReadTransaction(env);
    auto optView = store.reader(rtxn).get(id);
    REQUIRE(optView.has_value());
    CHECK(optView->property().duration() == std::chrono::minutes{3});
    CHECK(optView->metadata().trackNumber() == 1);
    CHECK(optView->metadata().trackTotal() == 10);
  }

  TEST_CASE("TrackStore - hot/cold updateHot and updateCold", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create initial hot+cold
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.duration = std::chrono::minutes{3};

    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Update hot only
    auto hotHeader2 = TrackHotHeader{};

    auto hotData2 = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData2.data(), &hotHeader2, sizeof(TrackHotHeader));

    auto wtxn3 = beginWriteTransaction(env);
    REQUIRE(store.writer(wtxn3).updateHot(id, hotData2));
    REQUIRE(wtxn3.commit());

    // Update cold only
    auto coldHeader2 = TrackColdHeader{};
    coldHeader2.duration = std::chrono::seconds{200};
    coldHeader2.trackNumber = 2;

    auto coldData2 = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData2.data(), &coldHeader2, sizeof(TrackColdHeader));

    auto wtxn4 = beginWriteTransaction(env);
    REQUIRE(store.writer(wtxn4).updateCold(
      id, coldData2.size(), [&](std::span<std::byte> buf) { std::ranges::copy(coldData2, buf.begin()); }));
    REQUIRE(wtxn4.commit());

    // Verify both persisted
    auto rtxn = beginReadTransaction(env);
    auto optView = store.reader(rtxn).get(id);
    REQUIRE(optView.has_value());
    CHECK(optView->property().duration() == std::chrono::seconds{200});
    CHECK(optView->metadata().trackNumber() == 2);
  }

  TEST_CASE("TrackStore - hot/cold remove", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create hot+cold
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Delete both
    auto wtxn3 = beginWriteTransaction(env);
    REQUIRE(store.writer(wtxn3).remove(id));
    REQUIRE(wtxn3.commit());

    // Verify both are gone
    auto rtxn = beginReadTransaction(env);
    auto optView = store.reader(rtxn).get(id);
    CHECK_FALSE(optView.has_value());
  }

  TEST_CASE("TrackStore - Writer get with LoadMode", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create hot+cold
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.duration = std::chrono::minutes{4};

    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Use Writer get with LoadMode (within same transaction context)
    auto wtxn3 = beginWriteTransaction(env);
    auto writer = store.writer(wtxn3);
    auto optHot = writer.get(id, TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optHot.has_value());
    CHECK(optHot->isHotValid());
    CHECK(!optHot->isColdValid());

    auto optCold = writer.get(id, TrackStore::Reader::LoadMode::Cold);
    REQUIRE(optCold.has_value());
    CHECK(!optCold->isHotValid());
    CHECK(optCold->isColdValid());
    CHECK(optCold->property().duration() == std::chrono::minutes{4});
    CHECK(optCold->coverArt().count() == 0);
  }

  TEST_CASE("TrackStore - unified TrackView iteration", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create multiple tracks
    for (std::int32_t i = 0; i < 3; ++i)
    {
      auto hotHeader = TrackHotHeader{};
      auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
      std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

      auto coldHeader = TrackColdHeader{};
      coldHeader.trackNumber = i + 1;
      auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
      std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

      auto wtxn2 = beginWriteTransaction(env);
      requireCreate(store.writer(wtxn2), hotData, coldData);
      REQUIRE(wtxn2.commit());
    }

    // Iterate via unified TrackView
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    std::int32_t count = 0;

    for (auto it = reader.begin(); it != reader.end(); ++it)
    {
      ++count;
    }

    CHECK(count == 3);
  }

  TEST_CASE("TrackStore - LoadMode::Hot iteration", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create tracks with hot+cold
    auto hotHeader = TrackHotHeader{};
    hotHeader.artistId = DictionaryId{1};
    hotHeader.albumId = DictionaryId{2};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.duration = std::chrono::minutes{3};
    coldHeader.trackNumber = 5;
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Iterate with Hot mode - should only load hot
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Hot);
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    CHECK(trackId == id);
    CHECK(trackView.isHotValid());
    CHECK(!trackView.isColdValid());
  }

  TEST_CASE("TrackStore - LoadMode::Cold iteration", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create tracks with hot+cold
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.duration = std::chrono::minutes{4};
    coldHeader.trackNumber = 3;
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Iterate with Cold mode
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Cold);
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    CHECK(trackId == id);
    CHECK(!trackView.isHotValid());
    CHECK(trackView.isColdValid());
  }

  TEST_CASE("TrackStore - LoadMode::Both iteration", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create tracks with hot+cold
    auto hotHeader = TrackHotHeader{};
    hotHeader.artistId = DictionaryId{1};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.duration = std::chrono::minutes{5};
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Iterate with Both mode - should load both
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Both);
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    CHECK(trackId == id);
    CHECK(trackView.isHotValid());
    CHECK(trackView.isColdValid());
    CHECK(trackView.property().duration() == std::chrono::minutes{5});
    CHECK(trackView.metadata().trackNumber() == 0); // default
  }

  TEST_CASE("TrackStore - LoadMode::Hot get by id", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create track
    auto hotHeader = TrackHotHeader{};
    hotHeader.artistId = DictionaryId{42};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Get with Hot mode
    auto rtxn = beginReadTransaction(env);
    auto optView = store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optView.has_value());
    CHECK(optView->isHotValid());
    CHECK(!optView->isColdValid());
    CHECK(optView->metadata().artistId() == DictionaryId{42});
  }

  TEST_CASE("TrackStore - LoadMode::Cold get by id", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create track
    auto hotHeader = TrackHotHeader{};
    hotHeader.artistId = DictionaryId{99};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.duration = std::chrono::minutes{6};
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = beginWriteTransaction(env);
    auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    // Get with Cold mode
    auto rtxn = beginReadTransaction(env);
    auto optView = store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Cold);
    REQUIRE(optView.has_value());
    CHECK(!optView->isHotValid());
    CHECK(optView->isColdValid());
    CHECK(optView->property().duration() == std::chrono::minutes{6});
  }

  TEST_CASE("TrackStore - LoadMode::Cold multi-record iteration", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create multiple tracks
    auto ids = std::vector<TrackId>{};

    for (std::int32_t i = 0; i < 5; ++i)
    {
      auto hotHeader = TrackHotHeader{};
      hotHeader.artistId = DictionaryId{static_cast<std::uint32_t>(i)};
      auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
      std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

      auto coldHeader = TrackColdHeader{};
      coldHeader.duration = std::chrono::seconds{180 + (i * 10)};
      coldHeader.trackNumber = i + 1;
      auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
      std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

      auto wtxn2 = beginWriteTransaction(env);
      auto [id, view] = requireCreate(store.writer(wtxn2), hotData, coldData);
      ids.push_back(id);
      REQUIRE(wtxn2.commit());
    }

    // Iterate with Cold mode and verify all records
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Cold);
    auto endIt = reader.end(TrackStore::Reader::LoadMode::Cold);

    std::int32_t count = 0;
    auto collectedIds = std::vector<TrackId>{};

    while (it != endIt)
    {
      auto&& [trackId, trackView] = *it;
      collectedIds.push_back(trackId);
      CHECK(!trackView.isHotValid());
      CHECK(trackView.isColdValid());
      ++it;
      ++count;
    }

    CHECK(count == 5);
    REQUIRE(collectedIds.size() == 5);

    // Verify all IDs match
    for (std::int32_t i = 0; i < 5; ++i)
    {
      CHECK(collectedIds[i].raw() == ids[i].raw());
    }
  }

  TEST_CASE("TrackStore - LoadMode::Cold empty iteration", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Iterate with Cold mode on empty store
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Cold);
    auto endIt = reader.end(TrackStore::Reader::LoadMode::Cold);

    CHECK(it == endIt);
  }

  TEST_CASE("TrackStore - iterator equality across modes", "[library][unit][track]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    REQUIRE(wtxn.commit());

    // Create a track
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = beginWriteTransaction(env);
    requireCreate(store.writer(wtxn2), hotData, coldData);
    REQUIRE(wtxn2.commit());

    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);

    // begin() with different modes must NOT equal each other
    auto coldBegin = reader.begin(TrackStore::Reader::LoadMode::Cold);
    auto hotBegin = reader.begin(TrackStore::Reader::LoadMode::Hot);
    auto bothBegin = reader.begin(TrackStore::Reader::LoadMode::Both);

    CHECK(coldBegin != hotBegin);
    CHECK(hotBegin != bothBegin);
    CHECK(coldBegin != bothBegin);

    // end() for different modes must NOT equal begin() of different mode
    CHECK(reader.end() != coldBegin);
    CHECK(reader.end() != hotBegin);
    CHECK(reader.end() != bothBegin);
  }

  TEST_CASE("TrackStore - read with missing cold data returns NotFound", "[library][unit][track]")
  {
    auto temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = beginWriteTransaction(env);

    auto store = TrackStore{openDatabase(wtxn, "tracks_hot"), openDatabase(wtxn, "tracks_cold")};
    auto writer = store.writer(wtxn);

    auto hotData = createStringData("hot_");
    auto coldData = createStringData("cold");
    auto [id, view] = requireCreate(writer, hotData, coldData);

    // Corrupt the DB by deleting cold data only
    auto coldDb = openDatabase(wtxn, "tracks_cold");
    REQUIRE(coldDb.writer(wtxn).del(id.raw()));
    REQUIRE(wtxn.commit());

    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);

    // Should return nullopt because cold data is missing
    auto optView = reader.get(id, TrackStore::Reader::LoadMode::Both);
    CHECK_FALSE(optView.has_value());
  }
} // namespace ao::library::test
