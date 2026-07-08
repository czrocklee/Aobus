// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
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

  TEST_CASE("ResourceStore - creates and reads resources", "[library][unit][resource]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ResourceStore{openDatabase(wtxn, "resources")};
    REQUIRE(wtxn.commit());

    // Create a resource
    auto const data = createStringData("hello");
    auto const& buffer = data;

    auto wtxn2 = beginWriteTransaction(env);
    auto idResult = store.writer(wtxn2).create(buffer);
    REQUIRE(idResult);
    auto const id = *idResult;
    CHECK(id > 0);
    REQUIRE(wtxn2.commit());

    // Read the resource
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    STATIC_REQUIRE(std::is_same_v<decltype(reader.maxKey()), ResourceId>);
    CHECK(reader.maxKey() == id);

    auto const optStored = reader.get(id);
    REQUIRE(optStored);
    CHECK(std::ranges::equal(*optStored, buffer));

    auto it = reader.begin();
    STATIC_REQUIRE(std::is_same_v<std::remove_cvref_t<decltype(it->first)>, ResourceId>);
    REQUIRE(it != reader.end());
    CHECK(it->first == id);
  }

  TEST_CASE("ResourceStore - deletes resources", "[library][unit][resource]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ResourceStore{openDatabase(wtxn, "resources")};
    REQUIRE(wtxn.commit());

    // Create a resource
    auto const data = createStringData("test");
    auto const& buffer = data;

    auto wtxn2 = beginWriteTransaction(env);
    auto idResult = store.writer(wtxn2).create(buffer);
    REQUIRE(idResult);
    auto const id = *idResult;
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

  TEST_CASE("ResourceStore - deduplicates matching resource data", "[library][unit][resource]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ResourceStore{openDatabase(wtxn, "resources")};
    REQUIRE(wtxn.commit());

    // Create first resource
    auto const data = createStringData("samedata");
    auto const& buffer = data;

    auto wtxn2 = beginWriteTransaction(env);
    auto id1Result = store.writer(wtxn2).create(buffer);
    REQUIRE(id1Result);
    auto const id1 = *id1Result;
    REQUIRE(wtxn2.commit());

    // Create same content again - should return same ID (deduplication)
    auto wtxn3 = beginWriteTransaction(env);
    auto id2Result = store.writer(wtxn3).create(buffer);
    REQUIRE(id2Result);
    auto const id2 = *id2Result;
    CHECK(id2 == id1);
    REQUIRE(wtxn3.commit());

    // Verify only one resource exists
    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    std::int32_t count = 0;

    for (auto it = reader.begin(); it != reader.end(); ++it)
    {
      ++count;
    }

    CHECK(count == 1);
  }

  TEST_CASE("ResourceStore - zero hash uses a valid ID", "[library][unit][resource]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto store = ResourceStore{openDatabase(wtxn, "resources")};
    REQUIRE(wtxn.commit());

    // XXH3-64 hashes these bytes to 0xf91404d400000000; the low 32 bits used
    // as the resource key are zero, which is reserved as the invalid ID.
    constexpr auto kData = std::array{std::byte{0xee}, std::byte{0xdc}, std::byte{0xc8}, std::byte{0xbf}};

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
    REQUIRE(wtxn2.commit());

    auto rtxn = beginReadTransaction(env);
    auto reader = store.reader(rtxn);
    auto const optStored = reader.get(id);
    REQUIRE(optStored);
    CHECK(std::ranges::equal(*optStored, kData));
  }
} // namespace ao::library::test
