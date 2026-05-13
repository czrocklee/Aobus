// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/ListLayout.h>
#include <ao/library/ListStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstring>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("ListStore - create and read", "[core][list]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = ListStore{Database{wtxn, "lists"}};
    wtxn.commit();

    // Create a list
    auto header = ListHeader{.trackIdsCount = 5};

    auto data = std::vector<std::byte>(sizeof(ListHeader));
    std::memcpy(data.data(), &header, sizeof(ListHeader));

    auto wtxn2 = WriteTransaction{env};
    auto const [id, view] = store.writer(wtxn2).create(data);
    // If create() failed, it would throw
    wtxn2.commit();

    // Read the list
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it != reader.end());
    REQUIRE((*it).first == id);
  }

  TEST_CASE("ListStore - read by id", "[core][list]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = ListStore{Database{wtxn, "lists"}};
    wtxn.commit();

    // Create a list
    auto header = ListHeader{.trackIdsCount = 10};

    auto const trackIdsSize = static_cast<std::size_t>(header.trackIdsCount) * sizeof(TrackId);
    auto data = std::vector<std::byte>(sizeof(ListHeader) + trackIdsSize);
    std::memcpy(data.data(), &header, sizeof(ListHeader));

    auto wtxn2 = WriteTransaction{env};
    auto const [id, view] = store.writer(wtxn2).create(data);
    wtxn2.commit();

    // Read by ID
    auto rtxn = ReadTransaction{env};
    auto const optFound = store.reader(rtxn).get(id);
    REQUIRE(optFound.has_value());
    auto const& found = *optFound;
    REQUIRE(found.tracks().size() == 10);
  }

  TEST_CASE("ListStore - delete", "[core][list]")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = ListStore{Database{wtxn, "lists"}};
    wtxn.commit();

    // Create a list
    auto header = ListHeader{};

    auto data = std::vector<std::byte>(sizeof(ListHeader));
    std::memcpy(data.data(), &header, sizeof(ListHeader));

    auto wtxn2 = WriteTransaction{env};
    auto const [id, view] = store.writer(wtxn2).create(data);
    wtxn2.commit();

    // Delete it
    auto wtxn3 = WriteTransaction{env};
    auto const deleted = store.writer(wtxn3).del(id);
    REQUIRE(deleted);
    wtxn3.commit();

    // Verify it's gone
    auto rtxn = ReadTransaction{env};
    auto reader = store.reader(rtxn);
    auto it = reader.begin();
    REQUIRE(it == reader.end());
  }
} // namespace ao::library::test
