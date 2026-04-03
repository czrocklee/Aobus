// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/ListLayout.h>
#include <rs/core/ListPayloadBuilder.h>
#include <rs/core/ListStore.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/lmdb/TestUtils.h>

#include <array>
#include <vector>

using rs::core::ListPayloadBuilder;
using rs::core::ListStore;
using rs::core::ListView;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::WriteTransaction;

TEST_CASE("ListPayloadBuilder - buildSmartList")
{
  auto payload = ListPayloadBuilder::buildSmartList("My Smart List", "A smart list", "@artist = 'Test'");
  auto view = ListView{payload};

  CHECK(view.isSmart() == true);
  CHECK(view.name() == "My Smart List");
  CHECK(view.filter() == "@artist = 'Test'");
}

TEST_CASE("ListPayloadBuilder - buildManualList")
{
  std::array<rs::core::TrackId, 3> const trackIds = {
    rs::core::TrackId{100}, rs::core::TrackId{200}, rs::core::TrackId{300}};

  auto payload = ListPayloadBuilder::buildManualList("My Manual List", "A manual list", trackIds);
  auto view = ListView{payload};

  CHECK(view.isSmart() == false);
  CHECK(view.name() == "My Manual List");
  CHECK(view.tracks().size() == 3);
  CHECK(view.tracks()[0] == rs::core::TrackId{100});
  CHECK(view.tracks()[1] == rs::core::TrackId{200});
  CHECK(view.tracks()[2] == rs::core::TrackId{300});
}

TEST_CASE("ListPayloadBuilder - buildManualList empty trackIds")
{
  auto payload = ListPayloadBuilder::buildManualList("Empty List", "No tracks", std::span<rs::core::TrackId const>{});
  auto view = ListView{payload};

  CHECK(view.isSmart() == false);
  CHECK(view.tracks().size() == 0);
}

TEST_CASE("ListPayloadBuilder - manual list round-trip through ListStore")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{wtxn, "lists"};
  wtxn.commit();

  auto const trackIds = std::array<rs::core::TrackId, 2>{rs::core::TrackId{42}, rs::core::TrackId{99}};
  auto payload = ListPayloadBuilder::buildManualList("RoundTrip Test", "Testing round-trip", trackIds);

  auto wtxn2 = WriteTransaction{env};
  auto [id, createdView] = store.writer(wtxn2).create(payload);
  wtxn2.commit();

  auto rtxn = ReadTransaction{env};
  auto optFound = store.reader(rtxn).get(id);
  REQUIRE(optFound.has_value());

  auto& found = *optFound;
  CHECK(found.isSmart() == false);
  CHECK(found.name() == "RoundTrip Test");
  CHECK(found.tracks().size() == 2);
  CHECK(found.tracks()[0] == rs::core::TrackId{42});
  CHECK(found.tracks()[1] == rs::core::TrackId{99});
}

TEST_CASE("ListPayloadBuilder - smart list round-trip through ListStore")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{wtxn, "lists"};
  wtxn.commit();

  auto payload = ListPayloadBuilder::buildSmartList("Smart RoundTrip", "Testing smart list round-trip", "@year > 2020");

  auto wtxn2 = WriteTransaction{env};
  auto [id, createdView] = store.writer(wtxn2).create(payload);
  wtxn2.commit();

  auto rtxn = ReadTransaction{env};
  auto optFound = store.reader(rtxn).get(id);
  REQUIRE(optFound.has_value());

  auto& found = *optFound;
  CHECK(found.isSmart() == true);
  CHECK(found.name() == "Smart RoundTrip");
  CHECK(found.filter() == "@year > 2020");
  CHECK(found.tracks().size() == 0);
}

TEST_CASE("ListPayloadBuilder - name and description offsets")
{
  auto payload = ListPayloadBuilder::buildManualList("Offset Test", "Desc Here", std::span<rs::core::TrackId const>{});
  auto view = ListView{payload};

  CHECK(view.name() == "Offset Test");
  CHECK(view.description() == "Desc Here");
}
