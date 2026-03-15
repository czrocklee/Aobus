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

#include <rs/core/ResourceStore.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/LmdbTestUtils.h>

#include <string_view>
#include <vector>

using rs::core::ResourceStore;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::test::TempDir;
using rs::lmdb::WriteTransaction;

TEST_CASE("ResourceStore - create and read", "[core][resource]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  ResourceStore store{wtxn, "resources"};
  wtxn.commit();

  // Create a resource
  std::vector<char> data = {'h', 'e', 'l', 'l', 'o'};
  auto buffer = boost::asio::const_buffer{data.data(), data.size()};

  WriteTransaction wtxn2(env);
  auto id = store.writer(wtxn2).create(buffer);
  REQUIRE(id > 0);
  wtxn2.commit();

  // Read the resource
  ReadTransaction rtxn(env);
  auto reader = store.reader(rtxn);
  auto it = reader.begin();
  REQUIRE(it != reader.end());
  REQUIRE(it->first == id);
}

TEST_CASE("ResourceStore - delete", "[core][resource]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  ResourceStore store{wtxn, "resources"};
  wtxn.commit();

  // Create a resource
  std::vector<char> data = {'t', 'e', 's', 't'};
  auto buffer = boost::asio::const_buffer{data.data(), data.size()};

  WriteTransaction wtxn2(env);
  auto id = store.writer(wtxn2).create(buffer);
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
