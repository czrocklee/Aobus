// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  namespace
  {
    DictionaryId requirePut(DictionaryStore& dictionary, WriteTransaction& transaction, std::string_view value)
    {
      return ao::test::requireValue(dictionary.put(transaction, value));
    }
  } // namespace

  TEST_CASE("DictionaryStore - put stores values retrievable by ID", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    // Store a value
    auto wtxn2 = beginWriteTransaction(env);
    auto const id = requirePut(dictionary, wtxn2, "test value");
    REQUIRE(wtxn2.commit());

    // Get by ID (using in-memory index)
    auto const result = dictionary.get(id);
    CHECK(result == "test value");
  }

  TEST_CASE("DictionaryStore - lookupId returns IDs for existing strings", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    requirePut(dictionary, wtxn, "artist1");
    REQUIRE(wtxn.commit());

    // Get ID by string
    [[maybe_unused]] auto const id = dictionary.lookupId("artist1");
    // If lookupId() failed, it would throw
  }

  TEST_CASE("DictionaryStore - contains reports whether strings exist", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    requirePut(dictionary, wtxn, "exists");
    REQUIRE(wtxn.commit());

    CHECK(dictionary.contains("exists"));
    CHECK(!dictionary.contains("not exists"));
  }

  TEST_CASE("DictionaryStore - put returns existing ID for duplicate strings", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    // Store a value
    auto wtxn2 = beginWriteTransaction(env);
    auto const id1 = requirePut(dictionary, wtxn2, "first");
    REQUIRE(wtxn2.commit());

    // Try to store same string again - returns existing ID
    auto wtxn3 = beginWriteTransaction(env);
    auto const id2 = requirePut(dictionary, wtxn3, "first");
    CHECK(id2 == id1);

    // Original value should still exist (using in-memory index)
    auto const result = dictionary.get(id1);
    CHECK(result == "first");
  }

  TEST_CASE("DictionaryStore - get throws on invalid IDs", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    requirePut(dictionary, wtxn, "first");
    REQUIRE(wtxn.commit());

    // ID 999 doesn't exist - should throw
    CHECK_THROWS(dictionary.get(DictionaryId{999}));
  }

  TEST_CASE("DictionaryStore - lookupId throws for missing strings", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    requirePut(dictionary, wtxn, "exists");
    REQUIRE(wtxn.commit());

    // Non-existent string should throw
    CHECK_THROWS(dictionary.lookupId("not exists"));
  }

  TEST_CASE("DictionaryStore - get returns the first valid ID", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    auto const id = requirePut(dictionary, wtxn, "first");
    REQUIRE(wtxn.commit());

    CHECK(id.raw() == 1);
    auto const result = dictionary.get(id);
    CHECK(result == "first");
  }

  TEST_CASE("DictionaryStore - get throws on out-of-bounds IDs", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    requirePut(dictionary, wtxn, "only one");
    REQUIRE(wtxn.commit());

    // ID 2 is out of bounds (only 1 is valid)
    CHECK_THROWS(dictionary.get(DictionaryId{2}));
  }

  TEST_CASE("DictionaryStore - getOrIntern returns new IDs for missing strings", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    // Reserve a non-existent string
    auto const id = dictionary.getOrIntern("new artist");
    CHECK(id.raw() == 1); // First getOrInternd ID is 0 (same as put)

    // contains should now return true (in-memory)
    CHECK(dictionary.contains("new artist"));
  }

  TEST_CASE("DictionaryStore - getOrIntern returns existing IDs for existing strings", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    requirePut(dictionary, wtxn, "existing");
    REQUIRE(wtxn.commit());

    // Reserve an existing string - should return the existing ID
    auto const id = dictionary.getOrIntern("existing");
    CHECK(id.raw() == 1); // First put uses ID 0

    CHECK(dictionary.contains("existing"));
  }

  TEST_CASE("DictionaryStore - put reuses IDs reserved by getOrIntern", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    // Reserve a string (not persisted)
    auto const internedId = dictionary.getOrIntern("Bach");

    // put() should return the same ID
    auto wtxn2 = beginWriteTransaction(env);
    auto const putId = requirePut(dictionary, wtxn2, "Bach");
    CHECK(putId == internedId);
  }

  TEST_CASE("DictionaryStore - getOrIntern assigns distinct IDs to multiple strings", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    auto const id1 = dictionary.getOrIntern("artist1");
    auto const id2 = dictionary.getOrIntern("artist2");
    auto const id3 = dictionary.getOrIntern("artist3");

    CHECK(id1 != id2);
    CHECK(id2 != id3);
    CHECK(id3 != id1);

    CHECK(dictionary.contains("artist1"));
    CHECK(dictionary.contains("artist2"));
    CHECK(dictionary.contains("artist3"));
  }

  TEST_CASE("DictionaryStore - put avoids IDs reserved for other strings", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    // Simulate scan populating some entries first
    auto wtxn2 = beginWriteTransaction(env);
    requirePut(dictionary, wtxn2, "artist_one");
    requirePut(dictionary, wtxn2, "album_one");
    REQUIRE(wtxn2.commit());

    // getOrIntern reserves "fav" (like query compiler does for "#fav" smart list)
    auto const reservedId = dictionary.getOrIntern("fav");
    CHECK(reservedId.raw() == 3); // after 2 puts, next is 3

    // put a DIFFERENT string "#fav" — must NOT collide with reserved "fav"
    auto wtxn3 = beginWriteTransaction(env);
    auto const putId = requirePut(dictionary, wtxn3, "#fav");
    REQUIRE(wtxn3.commit());

    CHECK(putId.raw() != reservedId.raw());
    CHECK(putId.raw() == 4); // must skip over reserved ID 3

    // Both strings should be independently resolvable
    CHECK(dictionary.get(reservedId) == "fav");
    CHECK(dictionary.get(putId) == "#fav");
    CHECK(dictionary.lookupId("fav").raw() == 3);
    CHECK(dictionary.lookupId("#fav").raw() == 4);
  }

  TEST_CASE("DictionaryStore - put persists IDs reserved for the same string", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    // Reserve a string
    auto const reservedId = dictionary.getOrIntern("fav");
    CHECK(reservedId.raw() == 1);

    // Put the SAME string — should reuse the reserved ID and persist it
    auto wtxn2 = beginWriteTransaction(env);
    auto const putId = requirePut(dictionary, wtxn2, "fav");
    REQUIRE(wtxn2.commit());

    CHECK(putId.raw() == reservedId.raw());
    CHECK(dictionary.get(putId) == "fav");

    // After restart simulation: reload dictionary from DB
    auto rtxn = beginReadTransaction(env);
    auto dict2 = DictionaryStore{openDatabase(rtxn, "dictionary"), rtxn};
    CHECK(dict2.get(putId) == "fav");
  }

  TEST_CASE("DictionaryStore - getOrDefault returns values for valid IDs", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    auto const id = requirePut(dictionary, wtxn, "hello");
    REQUIRE(wtxn.commit());

    CHECK(dictionary.getOrDefault(id) == "hello");
    CHECK(dictionary.getOrDefault(id, "fallback") == "hello");
  }

  TEST_CASE("DictionaryStore - getOrDefault returns defaults for invalid IDs", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    CHECK(dictionary.getOrDefault(kInvalidDictionaryId).empty());
    CHECK(dictionary.getOrDefault(kInvalidDictionaryId, "fallback") == "fallback");
    CHECK(dictionary.getOrDefault(DictionaryId{999}).empty());
  }

  TEST_CASE("DictionaryStore - loads databases with gapped IDs", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Manually create a gap in the database
    {
      auto wtxn = beginWriteTransaction(env);
      auto db = openDatabase(wtxn, "dictionary");
      auto writer = db.writer(wtxn);

      REQUIRE(writer.create(1, utility::bytes::view(std::string_view{"first"})));
      // SKIP ID 2
      REQUIRE(writer.create(3, utility::bytes::view(std::string_view{"third"})));
      // SKIP ID 4
      REQUIRE(writer.create(5, utility::bytes::view(std::string_view{"fifth"})));

      REQUIRE(wtxn.commit());
    }

    // Load DictionaryStore which should correctly handle the gaps via resize()
    auto rtxn = beginReadTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(rtxn, "dictionary"), rtxn};

    CHECK(dictionary.size() == 3);

    // Valid entries
    CHECK(dictionary.get(DictionaryId{1}) == "first");
    CHECK(dictionary.get(DictionaryId{3}) == "third");
    CHECK(dictionary.get(DictionaryId{5}) == "fifth");

    // Gapped entries should return empty strings but NOT throw out-of-bounds
    CHECK(dictionary.get(DictionaryId{2}).empty());
    CHECK(dictionary.get(DictionaryId{4}).empty());

    // Transparent index should also resolve correctly
    CHECK(dictionary.lookupId("first").raw() == 1);
    CHECK(dictionary.lookupId("fifth").raw() == 5);
  }

  TEST_CASE("DictionaryStore - recycles gapped IDs across restarts", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    // Manually create a gap in the database
    {
      auto wtxn = beginWriteTransaction(env);
      auto db = openDatabase(wtxn, "dictionary");
      auto writer = db.writer(wtxn);

      REQUIRE(writer.create(1, utility::bytes::view(std::string_view{"first"})));
      // SKIP ID 2
      REQUIRE(writer.create(3, utility::bytes::view(std::string_view{"third"})));

      REQUIRE(wtxn.commit());
    }

    // Load DictionaryStore which should discover ID 2 as a gap
    auto wtxn2 = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn2, "dictionary"), wtxn2};

    // Inserting a new string should RECYCLE ID 2 instead of appending to 4
    auto const newId1 = requirePut(dictionary, wtxn2, "recycled_id_2");
    CHECK(newId1.raw() == 2);

    // Inserting another new string should go to 4 since gaps are exhausted
    auto const newId2 = dictionary.getOrIntern("appended_id_4");
    CHECK(newId2.raw() == 4);

    // Inserting another should go to 5
    auto const newId3 = requirePut(dictionary, wtxn2, "appended_id_5");
    CHECK(newId3.raw() == 5);

    REQUIRE(wtxn2.commit());
  }

  TEST_CASE("DictionaryStore - keeps lookup indices valid after heavy reallocation", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    // Store the first string, capturing its ID
    auto wtxn2 = beginWriteTransaction(env);
    auto const firstId = requirePut(dictionary, wtxn2, "very_first_string");
    REQUIRE(wtxn2.commit());

    // Insert 10,000 unique short strings to force vector reallocation multiple times
    auto wtxn3 = beginWriteTransaction(env);

    for (std::int32_t i = 0; i < 10000; ++i)
    {
      requirePut(dictionary, wtxn3, "string_" + std::to_string(i));
    }

    REQUIRE(wtxn3.commit());

    // After massive reallocation, the old string_view in the transparent index should NOT dangle,
    // because we use DictionaryId in the hash set and vector element direct lookup.
    // So looking up "very_first_string" should succeed and return the correct ID.
    REQUIRE_NOTHROW(dictionary.lookupId("very_first_string"));
    CHECK(dictionary.lookupId("very_first_string") == firstId);

    // And get() should resolve correctly
    CHECK(dictionary.get(firstId) == "very_first_string");
  }

  // Run under TSan (./ao test --tsan) to verify the shared_mutex guards every
  // index, and under ASan to verify the deque keeps get()'s string_view valid
  // while a concurrent writer grows the backing storage.
  TEST_CASE("DictionaryStore - concurrent read and write access is race-free",
            "[library][unit][dictionary][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto dictionary = DictionaryStore{openDatabase(wtxn, "dictionary"), wtxn};
    REQUIRE(wtxn.commit());

    // Seed a stable set of entries that readers can always resolve. getOrIntern
    // stays in memory (no write transaction), so it is safe to call concurrently.
    constexpr std::int32_t kSeed = 256;
    auto seededIds = std::vector<DictionaryId>{};
    seededIds.reserve(kSeed);

    for (std::int32_t i = 0; i < kSeed; ++i)
    {
      seededIds.push_back(dictionary.getOrIntern("seed_" + std::to_string(i)));
    }

    auto failed = std::atomic{false};

    // Writer: keep interning fresh strings to force storage growth underneath
    // the readers.
    auto writer = std::jthread{[&](std::stop_token const& st)
                               {
                                 for (std::int32_t i = 0; !st.stop_requested(); ++i)
                                 {
                                   dictionary.getOrIntern("grow_" + std::to_string(i));
                                 }
                               }};

    // Readers: resolve seeded IDs/strings concurrently with the writer. The
    // returned string_view must stay valid across the writer's growth.
    auto reader = [&]
    {
      for (std::int32_t iter = 0; iter < 20000 && !failed.load(std::memory_order_relaxed); ++iter)
      {
        auto const index = static_cast<std::size_t>(iter) % seededIds.size();
        auto const id = seededIds[index];

        if (auto const expected = "seed_" + std::to_string(index);
            dictionary.get(id) != expected || !dictionary.contains(expected) || dictionary.lookupId(expected) != id)
        {
          failed.store(true, std::memory_order_relaxed);
        }

        std::ignore = dictionary.size();
      }
    };

    auto r1 = std::jthread{reader};
    auto r2 = std::jthread{reader};
    auto r3 = std::jthread{reader};

    r1.request_stop();
    r2.request_stop();
    r3.request_stop();

    if (r1.joinable())
    {
      r1.join();
    }

    if (r2.joinable())
    {
      r2.join();
    }

    if (r3.joinable())
    {
      r3.join();
    }

    writer.request_stop();

    if (writer.joinable())
    {
      writer.join();
    }

    CHECK(!failed.load());
  }
} // namespace ao::library::test
