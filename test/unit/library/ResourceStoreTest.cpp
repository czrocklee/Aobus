// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/ResourceStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("ResourceStore - create and read", "[library][unit][resource]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ResourceStore{openDatabase(wtxn, "resources")};
    wtxn.commit();

    // Create a resource
    auto const data = createStringData("hello");
    auto const& buffer = data;

    auto wtxn2 = beginWriteTransaction(env);
    auto idResult = store.writer(wtxn2).create(buffer);
    REQUIRE(idResult);
    auto const id = *idResult;
    REQUIRE(id > 0);
    wtxn2.commit();

    // Read the resource
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    STATIC_REQUIRE(std::is_same_v<decltype(reader.maxKey()), ResourceId>);
    CHECK(reader.maxKey() == id);

    auto const optStored = reader.get(id);
    REQUIRE(optStored.has_value());
    CHECK(std::ranges::equal(*optStored, buffer));

    auto it = reader.begin();
    STATIC_REQUIRE(std::is_same_v<std::remove_cvref_t<decltype(it->first)>, ResourceId>);
    REQUIRE(it != reader.end());
    REQUIRE(it->first == id);
  }

  TEST_CASE("ResourceStore - delete", "[library][unit][resource]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ResourceStore{openDatabase(wtxn, "resources")};
    wtxn.commit();

    // Create a resource
    auto const data = createStringData("test");
    auto const& buffer = data;

    auto wtxn2 = beginWriteTransaction(env);
    auto idResult = store.writer(wtxn2).create(buffer);
    REQUIRE(idResult);
    auto const id = *idResult;
    wtxn2.commit();

    // Delete it
    auto wtxn3 = beginWriteTransaction(env);
    REQUIRE(store.writer(wtxn3).remove(id));
    wtxn3.commit();

    // Verify it's gone
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it == reader.end());
  }

  TEST_CASE("ResourceStore - deduplication", "[library][unit][resource]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ResourceStore{openDatabase(wtxn, "resources")};
    wtxn.commit();

    // Create first resource
    auto const data = createStringData("samedata");
    auto const& buffer = data;

    auto wtxn2 = beginWriteTransaction(env);
    auto id1Result = store.writer(wtxn2).create(buffer);
    REQUIRE(id1Result);
    auto const id1 = *id1Result;
    wtxn2.commit();

    // Create same content again - should return same ID (deduplication)
    auto wtxn3 = beginWriteTransaction(env);
    auto id2Result = store.writer(wtxn3).create(buffer);
    REQUIRE(id2Result);
    auto const id2 = *id2Result;
    REQUIRE(id2 == id1);
    wtxn3.commit();

    // Verify only one resource exists
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    std::int32_t count = 0;

    for (auto it = reader.begin(); it != reader.end(); ++it)
    {
      ++count;
    }

    REQUIRE(count == 1);
  }

  TEST_CASE("ResourceStore - zero hash uses a valid ID", "[library][unit][resource]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ResourceStore{openDatabase(wtxn, "resources")};
    wtxn.commit();

    // FNV-1a 32-bit hashes these bytes to zero, which is reserved as the invalid ID.
    constexpr auto kData = std::array{std::byte{0xcc}, std::byte{0x24}, std::byte{0x31}, std::byte{0xc4}};

    auto wtxn2 = beginWriteTransaction(env);
    auto writer = store.writer(wtxn2);
    auto idResult = writer.create(kData);
    REQUIRE(idResult);
    auto const id = *idResult;
    auto duplicateIdResult = writer.create(kData);
    REQUIRE(duplicateIdResult);
    auto const duplicateId = *duplicateIdResult;
    CHECK(id != kInvalidResourceId);
    CHECK(duplicateId == id);
    wtxn2.commit();

    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto const optStored = reader.get(id);
    REQUIRE(optStored.has_value());
    CHECK(std::ranges::equal(*optStored, kData));
  }
} // namespace ao::library::test
