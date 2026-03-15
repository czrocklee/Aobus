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

#include <rs/core/Dictionary.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/LmdbTestUtils.h>

#include <string_view>

using rs::core::Dictionary;
using rs::core::DictionaryId;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::test::TempDir;
using rs::lmdb::WriteTransaction;

TEST_CASE("Dictionary - store and get", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  Database db{wtxn, "dict"};
  Dictionary dict{db};
  wtxn.commit();

  // Store a value
  WriteTransaction wtxn2(env);
  auto id = dict.put(wtxn2, "test value");
  REQUIRE(id.value() > 0);
  wtxn2.commit();

  // Get by ID
  ReadTransaction rtxn(env);
  auto result = dict.get(rtxn, id);
  REQUIRE(result == "test value");
}

TEST_CASE("Dictionary - getId", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  Database db{wtxn, "dict"};
  Dictionary dict{db};
  dict.put(wtxn, "artist1");
  wtxn.commit();

  // Get ID by string
  auto id = dict.getId("artist1");
  REQUIRE(id.value() > 0);
}

TEST_CASE("Dictionary - contains by id", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  Database db{wtxn, "dict"};
  Dictionary dict{db};
  auto id = dict.put(wtxn, "exists");
  wtxn.commit();

  ReadTransaction rtxn(env);
  REQUIRE(dict.contains(rtxn, id));
  REQUIRE(!dict.contains(rtxn, DictionaryId{999}));
}

TEST_CASE("Dictionary - contains by string", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  Database db{wtxn, "dict"};
  Dictionary dict{db};
  dict.put(wtxn, "exists");
  wtxn.commit();

  REQUIRE(dict.contains("exists"));
  REQUIRE(!dict.contains("not exists"));
}

TEST_CASE("Dictionary - put duplicate string returns existing ID", "[core][dictionary]")
{
  TempDir temp;
  auto env = Environment{};
  env.setMaxDatabases(20);
  env.open(temp.path().c_str(), MDB_CREATE, 0644);

  WriteTransaction wtxn(env);
  Database db{wtxn, "dict"};
  Dictionary dict{db};
  wtxn.commit();

  // Store a value
  WriteTransaction wtxn2(env);
  auto id1 = dict.put(wtxn2, "first");
  REQUIRE(id1.value() > 0);
  wtxn2.commit();

  // Try to store same string again - returns existing ID
  WriteTransaction wtxn3(env);
  auto id2 = dict.put(wtxn3, "first");
  REQUIRE(id2 == id1);

  // Original value should still exist
  ReadTransaction rtxn(env);
  auto result = dict.get(rtxn, id1);
  REQUIRE(result == "first");
}
