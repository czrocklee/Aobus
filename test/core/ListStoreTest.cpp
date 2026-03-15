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

#include <rs/core/ListLayout.h>
#include <rs/core/ListStore.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/LmdbTestUtils.h>

#include <cstring>
#include <vector>

using rs::core::ListHeader;
using rs::core::ListStore;
using rs::core::ListView;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::test::TempDir;
using rs::lmdb::WriteTransaction;

TEST_CASE("ListStore - create and read", "[core][list]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  ListStore store{wtxn, "lists"};
  wtxn.commit();

  // Create a list
  ListHeader header{};
  header.trackIdsCount = 5;

  std::vector<char> data(sizeof(ListHeader));
  std::memcpy(data.data(), &header, sizeof(ListHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data.data(), data.size());
  REQUIRE(id.value() > 0);
  wtxn2.commit();

  // Read the list
  ReadTransaction rtxn(env);
  auto reader = store.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE((*it).first == id);
}

TEST_CASE("ListStore - read by id", "[core][list]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  ListStore store{wtxn, "lists"};
  wtxn.commit();

  // Create a list
  ListHeader header{};
  header.trackIdsCount = 10;

  std::vector<char> data(sizeof(ListHeader));
  std::memcpy(data.data(), &header, sizeof(ListHeader));

  WriteTransaction wtxn2(env);
  auto [id, view] = store.writer(wtxn2).create(data.data(), data.size());
  wtxn2.commit();

  // Read by ID
  ReadTransaction rtxn(env);
  auto found = store.reader(rtxn)[id];
  REQUIRE(found.header()->trackIdsCount == 10);
}

TEST_CASE("ListStore - delete", "[core][list]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  ListStore store{wtxn, "lists"};
  wtxn.commit();

  // Create a list
  ListHeader header{};

  std::vector<char> data(sizeof(ListHeader));
  std::memcpy(data.data(), &header, sizeof(ListHeader));

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
