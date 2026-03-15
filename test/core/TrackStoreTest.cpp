/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

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
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create a track
  TrackHeader header{};
  header.fileSize = 1000;
  header.durationMs = 180000;
  header.trackNumber = 1;

  std::vector<char> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data.data(), data.size());
  REQUIRE(id.value() > 0);
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
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create a track
  TrackHeader header{};
  header.fileSize = 2000;

  std::vector<char> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data.data(), data.size());
  wtxn2.commit();

  // Read by ID
  ReadTransaction rtxn(env);
  auto found = store.reader(rtxn)[id];
  REQUIRE(found.property().fileSize() == 2000);
}

TEST_CASE("TrackStore - update", "[core][track]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create a track
  TrackHeader header{};
  header.fileSize = 1000;

  std::vector<char> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data.data(), data.size());
  wtxn2.commit();

  // Update the track
  TrackHeader header2{};
  header2.fileSize = 3000;

  std::vector<char> data2(sizeof(TrackHeader));
  std::memcpy(data2.data(), &header2, sizeof(TrackHeader));

  WriteTransaction wtxn3(env);
  auto updated = store.writer(wtxn3).update(id, data2.data(), data2.size());
  REQUIRE(updated.property().fileSize() == 3000);
  wtxn3.commit();
}

TEST_CASE("TrackStore - delete", "[core][track]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create a track
  TrackHeader header{};

  std::vector<char> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data.data(), data.size());
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
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  TrackStore store{wtxn, "tracks"};
  wtxn.commit();

  // Create multiple tracks - each should get unique ID
  TrackHeader header{};

  std::vector<char> data(sizeof(TrackHeader));
  std::memcpy(data.data(), &header, sizeof(TrackHeader));

  WriteTransaction wtxn2(env);
  auto [id1, view1] = store.writer(wtxn2).create(data.data(), data.size());
  auto [id2, view2] = store.writer(wtxn2).create(data.data(), data.size());
  auto [id3, view3] = store.writer(wtxn2).create(data.data(), data.size());
  wtxn2.commit();

  // All IDs should be unique
  REQUIRE(id1 != id2);
  REQUIRE(id2 != id3);
  REQUIRE(id1 != id3);
}
