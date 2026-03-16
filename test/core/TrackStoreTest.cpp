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

using rs::core::TrackHeader;
using rs::core::TrackStore;
using rs::core::TrackView;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::test::TempDir;
using rs::lmdb::WriteTransaction;

TEST_CASE("TrackStore - create and read", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create a track
  TrackHeader header{};
  header.fileSize = 1000;
  header.durationMs = 180000;
  header.trackNumber = 1;

  std::vector<std::byte> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data);
  // If create() failed, it would throw
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
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create a track
  TrackHeader header{};
  header.fileSize = 2000;

  std::vector<std::byte> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data);
  wtxn2.commit();

  // Read by ID
  ReadTransaction rtxn(env);
  auto optFound = store.reader(rtxn).get(id);
  REQUIRE(optFound.has_value());
  auto& found = *optFound;
  REQUIRE(found.property().fileSize() == 2000);
}

TEST_CASE("TrackStore - update", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create a track
  TrackHeader header{};
  header.fileSize = 1000;

  std::vector<std::byte> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data);
  wtxn2.commit();

  // Update the track
  TrackHeader header2{};
  header2.fileSize = 3000;

  std::vector<std::byte> data2(sizeof(TrackHeader));
  std::memcpy(data2.data(), &header2, sizeof(TrackHeader));

  WriteTransaction wtxn3(env);
  auto updated = store.writer(wtxn3).update(id, data2);
  REQUIRE(updated.property().fileSize() == 3000);
  wtxn3.commit();
}

TEST_CASE("TrackStore - delete", "[core][track]")
{
  TempDir temp;
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create a track
  TrackHeader header{};

  std::vector<std::byte> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data);
  wtxn2.commit();

  // Delete it
  WriteTransaction wtxn3(env);
  auto deleted = store.writer(wtxn3).del(id);
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
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create multiple tracks - each should get unique ID
  TrackHeader header{};

  std::vector<std::byte> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id1, view1] = store.writer(wtxn2).create(data);
  auto [id2, view2] = store.writer(wtxn2).create(data);
  auto [id3, view3] = store.writer(wtxn2).create(data);
  wtxn2.commit();

  // All IDs should be unique
  REQUIRE(id1 != id2);
  REQUIRE(id2 != id3);
  REQUIRE(id1 != id3);
}
