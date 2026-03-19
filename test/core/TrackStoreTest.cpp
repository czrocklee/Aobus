// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/TrackLayout.h>
#include <rs/core/TrackStore.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/LmdbTestUtils.h>

#include <cstring>
#include <vector>

using rs::core::TrackColdHeader;
using rs::core::TrackColdView;
using rs::core::TrackHotHeader;
using rs::core::TrackHotView;
using rs::core::TrackStore;
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

  // Empty cold data
  std::vector<std::byte> coldData(1);

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

  std::vector<std::byte> coldData(1);

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Read by ID
  ReadTransaction rtxn(env);
  auto optFound = store.reader(rtxn).hot().get(id);
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

  std::vector<std::byte> coldData(1);

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Update the track
  TrackHotHeader hotHeader2{};

  std::vector<std::byte> hotData2(sizeof(TrackHotHeader));
  std::memcpy(hotData2.data(), &hotHeader2, sizeof(TrackHotHeader));

  WriteTransaction wtxn3(env);
  auto updated = store.writer(wtxn3).updateHot(id, hotData2);
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

  std::vector<std::byte> coldData(1);

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Delete it
  WriteTransaction wtxn3(env);
  auto deleted = store.writer(wtxn3).delHotCold(id);
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

  std::vector<std::byte> coldData(1);

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
  coldHeader.fileSize = 1000;
  coldHeader.mtime = 1234567890;
  coldHeader.durationMs = 180000;
  coldHeader.trackNumber = 1;
  coldHeader.totalTracks = 10;

  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  // Create with hot+cold
  WriteTransaction wtxn2(env);
  auto [id, hotView] = store.writer(wtxn2).createHotCold(hotData, coldData);
  REQUIRE(id.value() >= 0);
  wtxn2.commit();

  // Verify hot data
  ReadTransaction rtxn(env);
  auto hotOpt = store.reader(rtxn).hot().get(id);
  REQUIRE(hotOpt.has_value());

  // Verify cold data
  auto coldOpt = store.reader(rtxn).cold().get(id);
  REQUIRE(coldOpt.has_value());
  REQUIRE(coldOpt->fileSize() == 1000);
  REQUIRE(coldOpt->mtime() == 1234567890);
  REQUIRE(coldOpt->durationMs() == 180000);
  REQUIRE(coldOpt->trackNumber() == 1);
  REQUIRE(coldOpt->totalTracks() == 10);
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
  coldHeader.fileSize = 1000;
  coldHeader.mtime = 1234567890;
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
  auto updatedHot = store.writer(wtxn3).updateHot(id, hotData2);
  wtxn3.commit();

  // Update cold only
  TrackColdHeader coldHeader2{};
  coldHeader2.fileSize = 2000;
  coldHeader2.mtime = 9876543210;
  coldHeader2.durationMs = 200000;
  coldHeader2.trackNumber = 2;

  std::vector<std::byte> coldData2(sizeof(TrackColdHeader));
  std::memcpy(coldData2.data(), &coldHeader2, sizeof(TrackColdHeader));

  WriteTransaction wtxn4(env);
  auto updatedCold = store.writer(wtxn4).updateCold(id, coldData2);
  REQUIRE(updatedCold.fileSize() == 2000);
  REQUIRE(updatedCold.mtime() == 9876543210);
  REQUIRE(updatedCold.durationMs() == 200000);
  REQUIRE(updatedCold.trackNumber() == 2);
  wtxn4.commit();

  // Verify both persisted
  ReadTransaction rtxn(env);
  auto hotOpt = store.reader(rtxn).hot().get(id);

  auto coldOpt = store.reader(rtxn).cold().get(id);
  REQUIRE(coldOpt->fileSize() == 2000);
  REQUIRE(coldOpt->mtime() == 9876543210);
  REQUIRE(coldOpt->durationMs() == 200000);
  REQUIRE(coldOpt->trackNumber() == 2);
}

TEST_CASE("TrackStore - hot/cold delHotCold", "[core][track]")
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
  auto deleted = store.writer(wtxn3).delHotCold(id);
  REQUIRE(deleted);
  wtxn3.commit();

  // Verify both are gone
  ReadTransaction rtxn(env);
  auto hotOpt = store.reader(rtxn).hot().get(id);
  REQUIRE(!hotOpt.has_value());

  auto coldOpt = store.reader(rtxn).cold().get(id);
  REQUIRE(!coldOpt.has_value());
}

TEST_CASE("TrackStore - hot/cold Writer getHot and getCold", "[core][track]")
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
  coldHeader.fileSize = 3000;
  coldHeader.durationMs = 240000;
  coldHeader.coverArtId = 42;

  std::vector<std::byte> coldData(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).createHotCold(hotData, coldData);
  wtxn2.commit();

  // Use Writer getHot/getCold (within same transaction context)
  WriteTransaction wtxn3(env);
  auto writer = store.writer(wtxn3);
  auto hotOpt = writer.getHot(id);
  REQUIRE(hotOpt.has_value());

  auto coldOpt = writer.getCold(id);
  REQUIRE(coldOpt.has_value());
  REQUIRE(coldOpt->fileSize() == 3000);
  REQUIRE(coldOpt->durationMs() == 240000);
  REQUIRE(coldOpt->coverArtId() == 42);
}

TEST_CASE("TrackStore - hot/cold proxy iteration", "[core][track]")
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

  // Iterate via hot proxy
  ReadTransaction rtxn(env);
  auto reader = store.reader(rtxn);
  int count = 0;
  for (auto it = reader.hot().begin(); it != reader.hot().end(); ++it) {
    ++count;
  }
  REQUIRE(count == 3);
}
