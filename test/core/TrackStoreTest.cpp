// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/TrackLayout.h>
#include <rs/core/TrackStore.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/TestUtils.h>

#include <cstring>
#include <vector>

using rs::core::DictionaryId;
using rs::core::TrackColdHeader;
using rs::core::TrackHotHeader;
using rs::core::TrackStore;
using rs::core::TrackView;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::WriteTransaction;

TEST_CASE("TrackStore - create and read", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create a track with hot+cold
  TrackHotHeader hotHeader{};

  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  // Empty cold data (padded to 4-byte alignment)
  std::vector<std::byte> coldData(4, std::byte{0});

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  // If createHotCold() failed, it would throw
  wtxn2.commit();

  // Read the track
  ReadTransaction rtxn(env);
  auto reader = store.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE((*it).first == id);
}

TEST_CASE("TrackStore - read by id", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create a track with hot+cold
  TrackHotHeader hotHeader{};

  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  std::vector<std::byte> coldData(4, std::byte{0});

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Read by ID
  ReadTransaction rtxn(env);
  auto optFound = store.reader(rtxn).get(id);
  REQUIRE(optFound.has_value());
}

TEST_CASE("TrackStore - update", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create a track
  TrackHotHeader hotHeader{};

  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  std::vector<std::byte> coldData(4, std::byte{0});

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Update the track
  TrackHotHeader hotHeader2{};

  std::vector<std::byte> hotData2(sizeof(TrackHotHeader));
  std::memcpy(hotData2.data(), &hotHeader2, sizeof(TrackHotHeader));

  WriteTransaction wtxn3(env);
  store.writer(wtxn3).updateHot(id, hotData2);
  wtxn3.commit();
}

TEST_CASE("TrackStore - delete", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create a track
  TrackHotHeader hotHeader{};

  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  std::vector<std::byte> coldData(4, std::byte{0});

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Delete it
  WriteTransaction wtxn3(env);
  auto deleted = store.writer(wtxn3).remove(id);
  REQUIRE(deleted);
  wtxn3.commit();

  // Verify it's gone
  ReadTransaction rtxn(env);
  auto reader = store.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it == reader.end());
}

TEST_CASE("TrackStore - create multiple tracks unique IDs", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create multiple tracks - each should get unique ID
  TrackHotHeader hotHeader{};

  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  std::vector<std::byte> coldData(4, std::byte{0});

  WriteTransaction wtxn2(env);
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
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create hot header
  TrackHotHeader hotHeader{};

  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  // Create cold header
  TrackColdHeader coldHeader{};
  coldHeader.fileSizeLo = static_cast<std::uint32_t>(1000 & 0xFFFFFFFF);
  coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1000) >> 32);
  coldHeader.mtimeLo = static_cast<std::uint32_t>(1234567890 & 0xFFFFFFFF);
  coldHeader.mtimeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1234567890) >> 32);
  coldHeader.durationMs = 180000;
  coldHeader.trackNumber = 1;
  coldHeader.totalTracks = 10;

  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  // Create with hot+cold
  WriteTransaction wtxn2(env);
  auto [id, hotView] = store.writer(wtxn2).createHotCold(hotData, coldData);
// REQUIRE(id.value() >= 0);
  wtxn2.commit();

  // Verify hot and cold data
  ReadTransaction rtxn(env);
  auto trackOpt = store.reader(rtxn).get(id);
  REQUIRE(trackOpt.has_value());
  REQUIRE(trackOpt->property().fileSize() == 1000);
  REQUIRE(trackOpt->property().mtime() == 1234567890);
  REQUIRE(trackOpt->property().durationMs() == 180000);
  REQUIRE(trackOpt->metadata().trackNumber() == 1);
  REQUIRE(trackOpt->metadata().totalTracks() == 10);
}

TEST_CASE("TrackStore - hot/cold updateHot and updateCold", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create initial hot+cold
  TrackHotHeader hotHeader{};

  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  TrackColdHeader coldHeader{};
  coldHeader.fileSizeLo = static_cast<std::uint32_t>(1000 & 0xFFFFFFFF);
  coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1000) >> 32);
  coldHeader.mtimeLo = static_cast<std::uint32_t>(1234567890 & 0xFFFFFFFF);
  coldHeader.mtimeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1234567890) >> 32);
  coldHeader.durationMs = 180000;

  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Update hot only
  TrackHotHeader hotHeader2{};

  std::vector<std::byte> hotData2(sizeof(TrackHotHeader));
  std::memcpy(hotData2.data(), &hotHeader2, sizeof(TrackHotHeader));

  WriteTransaction wtxn3(env);
  store.writer(wtxn3).updateHot(id, hotData2);
  wtxn3.commit();

  // Update cold only
  TrackColdHeader coldHeader2{};
  coldHeader2.fileSizeLo = static_cast<std::uint32_t>(2000 & 0xFFFFFFFF);
  coldHeader2.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(2000) >> 32);
  coldHeader2.mtimeLo = static_cast<std::uint32_t>(9876543210 & 0xFFFFFFFF);
  coldHeader2.mtimeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(9876543210) >> 32);
  coldHeader2.durationMs = 200000;
  coldHeader2.trackNumber = 2;

  std::vector<std::byte> coldData2(sizeof(TrackColdHeader));
  std::memcpy(coldData2.data(), &coldHeader2, sizeof(TrackColdHeader));

  WriteTransaction wtxn4(env);
  store.writer(wtxn4).updateCold(id, coldData2);
  wtxn4.commit();

  // Verify both persisted
  ReadTransaction rtxn(env);
  auto trackOpt = store.reader(rtxn).get(id);
  REQUIRE(trackOpt.has_value());
  REQUIRE(trackOpt->property().fileSize() == 2000);
  REQUIRE(trackOpt->property().mtime() == 9876543210);
  REQUIRE(trackOpt->property().durationMs() == 200000);
  REQUIRE(trackOpt->metadata().trackNumber() == 2);
}

TEST_CASE("TrackStore - hot/cold remove", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create hot+cold
  TrackHotHeader hotHeader{};
  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  TrackColdHeader coldHeader{};
  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Delete both
  WriteTransaction wtxn3(env);
  auto deleted = store.writer(wtxn3).remove(id);
  REQUIRE(deleted);
  wtxn3.commit();

  // Verify both are gone
  ReadTransaction rtxn(env);
  auto trackOpt = store.reader(rtxn).get(id);
  REQUIRE(!trackOpt.has_value());
}

TEST_CASE("TrackStore - Writer get with LoadMode", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create hot+cold
  TrackHotHeader hotHeader{};

  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  TrackColdHeader coldHeader{};
  coldHeader.fileSizeLo = static_cast<std::uint32_t>(3000 & 0xFFFFFFFF);
  coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(3000) >> 32);
  coldHeader.durationMs = 240000;
  coldHeader.coverArtId = 42;

  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Use Writer get with LoadMode (within same transaction context)
  WriteTransaction wtxn3(env);
  auto writer = store.writer(wtxn3);
  auto hotOpt = writer.get(id, TrackStore::Reader::LoadMode::Hot);
  REQUIRE(hotOpt.has_value());
  REQUIRE(hotOpt->isHotValid());
  REQUIRE(!hotOpt->isColdValid());

  auto coldOpt = writer.get(id, TrackStore::Reader::LoadMode::Cold);
  REQUIRE(coldOpt.has_value());
  REQUIRE(!coldOpt->isHotValid());
  REQUIRE(coldOpt->isColdValid());
  REQUIRE(coldOpt->property().fileSize() == 3000);
  REQUIRE(coldOpt->property().durationMs() == 240000);
  REQUIRE(coldOpt->metadata().coverArtId() == 42);
}

TEST_CASE("TrackStore - unified TrackView iteration", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create multiple tracks
  for (int i = 0; i < 3; ++i) {
    TrackHotHeader hotHeader{};
    std::vector<std::byte> hotData(sizeof(TrackHotHeader));
    std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

    TrackColdHeader coldHeader{};
    coldHeader.trackNumber = i + 1;
    std::vector<std::byte> coldData(sizeof(TrackColdHeader));
    std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

    WriteTransaction wtxn2(env);
    store.writer(wtxn2).createHotCold(hotData, coldData);
    wtxn2.commit();
  }

  // Iterate via unified TrackView
  ReadTransaction rtxn(env);
  auto reader = store.reader(rtxn);
  int count = 0;
  for (auto it = reader.begin(); it != reader.end(); ++it) {
    ++count;
  }
  REQUIRE(count == 3);
}

TEST_CASE("TrackStore - LoadMode::Hot iteration", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create tracks with hot+cold
  TrackHotHeader hotHeader{};
  hotHeader.artistId = DictionaryId{1};
  hotHeader.albumId = DictionaryId{2};
  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  TrackColdHeader coldHeader{};
  coldHeader.fileSizeLo = static_cast<std::uint32_t>(1000 & 0xFFFFFFFF);
  coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1000) >> 32);
  coldHeader.durationMs = 180000;
  coldHeader.trackNumber = 5;
  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Iterate with Hot mode - should only load hot
  ReadTransaction rtxn(env);
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
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create tracks with hot+cold
  TrackHotHeader hotHeader{};
  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  TrackColdHeader coldHeader{};
  coldHeader.fileSizeLo = static_cast<std::uint32_t>(2000 & 0xFFFFFFFF);
  coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(2000) >> 32);
  coldHeader.durationMs = 240000;
  coldHeader.trackNumber = 3;
  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Iterate with Cold mode
  ReadTransaction rtxn(env);
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
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create tracks with hot+cold
  TrackHotHeader hotHeader{};
  hotHeader.artistId = DictionaryId{1};
  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  TrackColdHeader coldHeader{};
  coldHeader.fileSizeLo = static_cast<std::uint32_t>(3000 & 0xFFFFFFFF);
  coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(3000) >> 32);
  coldHeader.durationMs = 300000;
  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Iterate with Both mode - should load both
  ReadTransaction rtxn(env);
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
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create track
  TrackHotHeader hotHeader{};
  hotHeader.artistId = DictionaryId{42};
  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  TrackColdHeader coldHeader{};
  coldHeader.fileSizeLo = static_cast<std::uint32_t>(5000 & 0xFFFFFFFF);
  coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(5000) >> 32);
  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Get with Hot mode
  ReadTransaction rtxn(env);
  auto optTrack = store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Hot);
  REQUIRE(optTrack.has_value());
  REQUIRE(optTrack->isHotValid());
  REQUIRE(!optTrack->isColdValid());
  REQUIRE(optTrack->metadata().artistId() == DictionaryId{42});
}

TEST_CASE("TrackStore - LoadMode::Cold get by id", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks_hot", "tracks_cold"};
  wtxn.commit();

  // Create track
  TrackHotHeader hotHeader{};
  hotHeader.artistId = DictionaryId{99};
  std::vector<std::byte> hotData(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  TrackColdHeader coldHeader{};
  coldHeader.fileSizeLo = static_cast<std::uint32_t>(6000 & 0xFFFFFFFF);
  coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(6000) >> 32);
  coldHeader.durationMs = 360000;
  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Get with Cold mode
  ReadTransaction rtxn(env);
  auto optTrack = store.reader(rtxn).get(id, TrackStore::Reader::LoadMode::Cold);
  REQUIRE(optTrack.has_value());
  REQUIRE(!optTrack->isHotValid());
  REQUIRE(optTrack->isColdValid());
  REQUIRE(optTrack->property().fileSize() == 6000);
  REQUIRE(optTrack->property().durationMs() == 360000);
}