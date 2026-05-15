// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <lmdb.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("TrackStore - create and read", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create a track with hot+cold
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    // Empty cold data (padded to 4-byte alignment)
    auto coldData = std::vector<std::byte>(4, std::byte{0});

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    // If createHotCold() failed, it would throw
    wtxn2.commit();

    // Read the track
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE((*it).first == id);
  }

  TEST_CASE("TrackStore - read by id", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create a track with hot+cold
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldData = std::vector<std::byte>(4, std::byte{0});

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Read by ID
    auto rtxn = ReadTransaction{env};
    auto optFound = store.reader(rtxn).get(id);
    REQUIRE(optFound.has_value());
  }

  TEST_CASE("TrackStore - update", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create a track
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldData = std::vector<std::byte>(4, std::byte{0});

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Update the track
    auto hotHeader2 = TrackHotHeader{};

    auto hotData2 = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData2.data(), &hotHeader2, sizeof(TrackHotHeader));

    auto wtxn3 = WriteTransaction{env};
    store.writer(wtxn3).updateHot(id, hotData2);
    wtxn3.commit();
  }

  TEST_CASE("TrackStore - delete", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create a track
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldData = std::vector<std::byte>(4, std::byte{0});

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Delete it
    auto wtxn3 = WriteTransaction{env};
    auto deleted = store.writer(wtxn3).remove(id);
    REQUIRE(deleted);
    wtxn3.commit();

    // Verify it's gone
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it == reader.end());
  }

  TEST_CASE("TrackStore - create multiple tracks unique IDs", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create multiple tracks - each should get unique ID
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldData = std::vector<std::byte>(4, std::byte{0});

    auto wtxn2 = WriteTransaction{env};
    auto [id1, view1] = store.writer(wtxn2).createHotCold(hotData, coldData);
    auto [id2, view2] = store.writer(wtxn2).createHotCold(hotData, coldData);
    auto [id3, view3] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // All IDs should be unique
    REQUIRE(id1 != id2);
    REQUIRE(id2 != id3);
    REQUIRE(id1 != id3);
  }

  TEST_CASE("TrackStore - hot/cold createHotCold", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create hot header
    auto hotHeader = TrackHotHeader{};

    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    // Create cold header
    auto coldHeader = TrackColdHeader{};
    coldHeader.fileSizeLo = static_cast<std::uint32_t>(1000 & 0xFFFFFFFF);
    coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1000) >> 32);
    coldHeader.mtimeLo = static_cast<std::uint32_t>(1234567890 & 0xFFFFFFFF);
    coldHeader.mtimeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1234567890) >> 32);
    coldHeader.durationMs = 180000;
    coldHeader.trackNumber = 1;
    coldHeader.totalTracks = 10;

    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    // Create with hot+cold
    auto wtxn2 = WriteTransaction{env};
    auto [id, hotView] = store.writer(wtxn2).createHotCold(hotData, coldData);
    // REQUIRE(id.value() >= 0);
    wtxn2.commit();

    // Verify hot and cold data
    auto rtxn = ReadTransaction{env};
    auto optTrack = store.reader(rtxn).get(id);
    REQUIRE(optTrack.has_value());
    REQUIRE(optTrack->property().fileSize() == 1000);
    REQUIRE(optTrack->property().mtime() == 1234567890);
    REQUIRE(optTrack->property().durationMs() == 180000);
    REQUIRE(optTrack->metadata().trackNumber() == 1);
    REQUIRE(optTrack->metadata().totalTracks() == 10);
  }

  TEST_CASE("TrackStore - hot/cold updateHot and updateCold", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create initial hot+cold
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.fileSizeLo = static_cast<std::uint32_t>(1000 & 0xFFFFFFFF);
    coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1000) >> 32);
    coldHeader.mtimeLo = static_cast<std::uint32_t>(1234567890 & 0xFFFFFFFF);
    coldHeader.mtimeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1234567890) >> 32);
    coldHeader.durationMs = 180000;

    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Update hot only
    auto hotHeader2 = TrackHotHeader{};

    auto hotData2 = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData2.data(), &hotHeader2, sizeof(TrackHotHeader));

    auto wtxn3 = WriteTransaction{env};
    store.writer(wtxn3).updateHot(id, hotData2);
    wtxn3.commit();

    // Update cold only
    auto coldHeader2 = TrackColdHeader{};
    coldHeader2.fileSizeLo = static_cast<std::uint32_t>(2000 & 0xFFFFFFFF);
    coldHeader2.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(2000) >> 32);
    coldHeader2.mtimeLo = static_cast<std::uint32_t>(9876543210 & 0xFFFFFFFF);
    coldHeader2.mtimeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(9876543210) >> 32);
    coldHeader2.durationMs = 200000;
    coldHeader2.trackNumber = 2;

    auto coldData2 = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData2.data(), &coldHeader2, sizeof(TrackColdHeader));

    auto wtxn4 = WriteTransaction{env};
    store.writer(wtxn4).updateCold(id, coldData2);
    wtxn4.commit();

    // Verify both persisted
    auto rtxn = ReadTransaction{env};
    auto optTrack = store.reader(rtxn).get(id);
    REQUIRE(optTrack.has_value());
    REQUIRE(optTrack->property().fileSize() == 2000);
    REQUIRE(optTrack->property().mtime() == 9876543210);
    REQUIRE(optTrack->property().durationMs() == 200000);
    REQUIRE(optTrack->metadata().trackNumber() == 2);
  }

  TEST_CASE("TrackStore - hot/cold remove", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create hot+cold
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Delete both
    auto wtxn3 = WriteTransaction{env};
    auto deleted = store.writer(wtxn3).remove(id);
    REQUIRE(deleted);
    wtxn3.commit();

    // Verify both are gone
    auto rtxn = ReadTransaction{env};
    auto optTrack = store.reader(rtxn).get(id);
    REQUIRE(!optTrack.has_value());
  }

  TEST_CASE("TrackStore - Writer get with LoadMode", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create hot+cold
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.fileSizeLo = static_cast<std::uint32_t>(3000 & 0xFFFFFFFF);
    coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(3000) >> 32);
    coldHeader.durationMs = 240000;
    coldHeader.coverArtId = 42;

    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Use Writer get with LoadMode (within same transaction context)
    auto wtxn3 = WriteTransaction{env};
    auto writer = store.writer(wtxn3);
    auto optHot = writer.get(id, TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optHot.has_value());
    REQUIRE(optHot->isHotValid());
    REQUIRE(!optHot->isColdValid());

    auto optCold = writer.get(id, TrackStore::Reader::LoadMode::Cold);
    REQUIRE(optCold.has_value());
    REQUIRE(!optCold->isHotValid());
    REQUIRE(optCold->isColdValid());
    REQUIRE(optCold->property().fileSize() == 3000);
    REQUIRE(optCold->property().durationMs() == 240000);
    REQUIRE(optCold->metadata().coverArtId() == 42);
  }

  TEST_CASE("TrackStore - unified TrackView iteration", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create multiple tracks
    for (int i = 0; i < 3; ++i)
    {
      auto hotHeader = TrackHotHeader{};
      auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
      std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

      auto coldHeader = TrackColdHeader{};
      coldHeader.trackNumber = i + 1;
      auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
      std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

      auto wtxn2 = WriteTransaction{env};
      store.writer(wtxn2).createHotCold(hotData, coldData);
      wtxn2.commit();
    }

    // Iterate via unified TrackView
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    int count = 0;

    for (auto it = reader.begin(); it != reader.end(); ++it)
    {
      ++count;
    }

    REQUIRE(count == 3);
  }

  TEST_CASE("TrackStore - LoadMode::Hot iteration", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create tracks with hot+cold
    auto hotHeader = TrackHotHeader{};
    hotHeader.artistId = DictionaryId{1};
    hotHeader.albumId = DictionaryId{2};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.fileSizeLo = static_cast<std::uint32_t>(1000 & 0xFFFFFFFF);
    coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1000) >> 32);
    coldHeader.durationMs = 180000;
    coldHeader.trackNumber = 5;
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Iterate with Hot mode - should only load hot
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Hot);
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    REQUIRE(trackId == id);
    REQUIRE(trackView.isHotValid());
    REQUIRE(!trackView.isColdValid());
  }

  TEST_CASE("TrackStore - LoadMode::Cold iteration", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create tracks with hot+cold
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.fileSizeLo = static_cast<std::uint32_t>(2000 & 0xFFFFFFFF);
    coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(2000) >> 32);
    coldHeader.durationMs = 240000;
    coldHeader.trackNumber = 3;
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Iterate with Cold mode
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Cold);
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    REQUIRE(trackId == id);
    REQUIRE(!trackView.isHotValid());
    REQUIRE(trackView.isColdValid());
  }

  TEST_CASE("TrackStore - LoadMode::Both iteration", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create tracks with hot+cold
    auto hotHeader = TrackHotHeader{};
    hotHeader.artistId = DictionaryId{1};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.fileSizeLo = static_cast<std::uint32_t>(3000 & 0xFFFFFFFF);
    coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(3000) >> 32);
    coldHeader.durationMs = 300000;
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Iterate with Both mode - should load both
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Both);
    REQUIRE(it != reader.end());
    auto&& [trackId, trackView] = *it;
    REQUIRE(trackId == id);
    REQUIRE(trackView.isHotValid());
    REQUIRE(trackView.isColdValid());
    REQUIRE(trackView.property().fileSize() == 3000);
    REQUIRE(trackView.metadata().trackNumber() == 0); // default
  }

  TEST_CASE("TrackStore - LoadMode::Hot get by id", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create track
    auto hotHeader = TrackHotHeader{};
    hotHeader.artistId = DictionaryId{42};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.fileSizeLo = static_cast<std::uint32_t>(5000 & 0xFFFFFFFF);
    coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(5000) >> 32);
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Get with Hot mode
    auto rtxn = ReadTransaction{env};
    auto optTrack = store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrack.has_value());
    REQUIRE(optTrack->isHotValid());
    REQUIRE(!optTrack->isColdValid());
    REQUIRE(optTrack->metadata().artistId() == DictionaryId{42});
  }

  TEST_CASE("TrackStore - LoadMode::Cold get by id", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create track
    auto hotHeader = TrackHotHeader{};
    hotHeader.artistId = DictionaryId{99};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    coldHeader.fileSizeLo = static_cast<std::uint32_t>(6000 & 0xFFFFFFFF);
    coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(6000) >> 32);
    coldHeader.durationMs = 360000;
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = WriteTransaction{env};
    auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    // Get with Cold mode
    auto rtxn = ReadTransaction{env};
    auto optTrack = store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Cold);
    REQUIRE(optTrack.has_value());
    REQUIRE(!optTrack->isHotValid());
    REQUIRE(optTrack->isColdValid());
    REQUIRE(optTrack->property().fileSize() == 6000);
    REQUIRE(optTrack->property().durationMs() == 360000);
  }

  TEST_CASE("TrackStore - LoadMode::Cold multi-record iteration", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create multiple tracks
    auto ids = std::vector<TrackId>{};

    for (int i = 0; i < 5; ++i)
    {
      auto hotHeader = TrackHotHeader{};
      hotHeader.artistId = DictionaryId{static_cast<std::uint32_t>(i)};
      auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
      std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

      auto coldHeader = TrackColdHeader{};
      coldHeader.fileSizeLo = static_cast<std::uint32_t>((1000 + i) & 0xFFFFFFFF);
      coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1000 + i) >> 32);
      coldHeader.durationMs = static_cast<std::uint32_t>(180000 + (i * 10000));
      coldHeader.trackNumber = i + 1;
      auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
      std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

      auto wtxn2 = WriteTransaction{env};
      auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
      ids.push_back(id);
      wtxn2.commit();
    }

    // Iterate with Cold mode and verify all records
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Cold);
    auto endIt = reader.end(TrackStore::Reader::LoadMode::Cold);

    auto count = 0;
    auto collectedIds = std::vector<TrackId>{};

    while (it != endIt)
    {
      auto&& [trackId, trackView] = *it;
      collectedIds.push_back(trackId);
      REQUIRE(!trackView.isHotValid());
      REQUIRE(trackView.isColdValid());
      ++it;
      ++count;
    }

    REQUIRE(count == 5);
    REQUIRE(collectedIds.size() == 5);

    // Verify all IDs match
    for (int i = 0; i < 5; ++i)
    {
      REQUIRE(collectedIds[i].value() == ids[i].value());
    }
  }

  TEST_CASE("TrackStore - LoadMode::Cold empty iteration", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Iterate with Cold mode on empty store
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin(TrackStore::Reader::LoadMode::Cold);
    auto endIt = reader.end(TrackStore::Reader::LoadMode::Cold);

    REQUIRE(it == endIt);
  }

  TEST_CASE("TrackStore - iterator equality across modes", "[core][track]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = TrackStore{Database{wtxn, "tracks_hot"}, Database{wtxn, "tracks_cold"}};
    wtxn.commit();

    // Create a track
    auto hotHeader = TrackHotHeader{};
    auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    auto coldHeader = TrackColdHeader{};
    auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    auto wtxn2 = WriteTransaction{env};
    store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();

    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);

    // begin() with different modes must NOT equal each other
    auto coldBegin = reader.begin(TrackStore::Reader::LoadMode::Cold);
    auto hotBegin = reader.begin(TrackStore::Reader::LoadMode::Hot);
    auto bothBegin = reader.begin(TrackStore::Reader::LoadMode::Both);

    REQUIRE(coldBegin != hotBegin);
    REQUIRE(hotBegin != bothBegin);
    REQUIRE(coldBegin != bothBegin);

    // end() for different modes must NOT equal begin() of different mode
    REQUIRE(reader.end() != coldBegin);
    REQUIRE(reader.end() != hotBegin);
    REQUIRE(reader.end() != bothBegin);
  }
} // namespace ao::library::test
